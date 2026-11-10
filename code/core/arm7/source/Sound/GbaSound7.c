#include "common.h"
#include <libtwl/ipc/ipcSync.h>
#include "GbaTimer.h"
#include "GbSound.h"
#include "GbaSound.h"
#include "GbaSoundDefinitions.h"
#include "GbaSound7.h"

gbas_shared_t* gSoundSharedData;
static gbat_t sTimers[2];
static gbas_direct_channel7_t sDirectChannels[2];
static u16 sSoundCntH;
static bool sPaused;
static u16 sSoundBias;
static volatile u32 sPcmPlan;
// Computer-only paired experiment. OFF/ON NDS differ only in this initialized
// ARM7 data word; no public setting and no shared-memory layout changes.
volatile u32 gPcmAdaptEnabled = 1;


static s8 updateDirectChannel(gbas_direct_channel_t* channel, gbas_direct_channel7_t* channel7, u32 timerOverflows)
{
    u32 samples = channel7->curPlaySamples;
    // no overflows, or too many overflows
    if (timerOverflows == 0 || timerOverflows > 10)
    {
        return (s8)samples;
    }

    for (u32 i = timerOverflows; i > 0; --i)
    {
        u32 readOffset = channel->readOffset;
        u32 fifoCount = (channel->writeOffset - readOffset);
        if (!(fifoCount & 4) && !channel->dmaRequest)
        {
            channel->dmaRequest = true;
            ipc_triggerArm7Irq();
        }
        if (channel7->curPlaySampleCount != 0)
        {
            samples >>= 8;
            channel7->curPlaySampleCount--;
        }
        else if (__builtin_expect((fifoCount & 7) != 0, true))
        {
            samples = channel->fifo[readOffset];
            channel7->curPlaySampleCount = 3;
            channel->readOffset = (readOffset + 1) & 7;
        }
    }
    channel7->curPlaySamples = samples;

    return (s8)samples;
}

static inline s16 clampSample(int inSample)
{
    int outSample = inSample << 23;
    if (inSample != (outSample >> 23))
        outSample = 0x7FFFFFFF ^ (inSample >> 31);
    return outSample >> 16;
}


static void refreshAdaptMode(void);

void gbas_setSoundBiasLo(u8 value)
{
    sSoundBias = (sSoundBias & 0xFF00) | value;
    refreshAdaptMode();
}
void gbas_setSoundBiasHi(u8 value)
{
    sSoundBias = (sSoundBias & 0x00FF) | ((u16)value << 8);
    refreshAdaptMode();
}

static inline bool adaptiveTimerSupported(const gbat_t* timer)
{
    if (!(timer->control & GBAT_CONTROL_ENABLED)) return true;
    return !(timer->control & (GBAT_CONTROL_SLAVE | GBAT_CONTROL_PRESCALE_MASK))
        && (0x10000u - timer->reload) >= 512;
}

static inline bool adaptiveTimerNonIntegral(const gbat_t* timer)
{
    return (timer->control & GBAT_CONTROL_ENABLED)
        && ((0x10000u - timer->reload) & 511);
}

static void refreshAdaptMode(void)
{
    u32 plan = 0;
    u32 mode = sSoundBias >> 14;
    if (gPcmAdaptEnabled && mode && (sSoundBias & 0x3FF) == 0x200)
    {
        u32 h = sSoundCntH;
        u32 ta = !!(h & GBA_SOUNDCNT_H_DIRECT_A_TIMER_1);
        u32 tb = !!(h & GBA_SOUNDCNT_H_DIRECT_B_TIMER_1);
        bool routeA = h & (GBA_SOUNDCNT_H_DIRECT_A_ENABLE_LEFT | GBA_SOUNDCNT_H_DIRECT_A_ENABLE_RIGHT);
        bool routeB = h & (GBA_SOUNDCNT_H_DIRECT_B_ENABLE_LEFT | GBA_SOUNDCNT_H_DIRECT_B_ENABLE_RIGHT);
        if ((!routeA || adaptiveTimerSupported(&sTimers[ta])) &&
            (!routeB || adaptiveTimerSupported(&sTimers[tb])) &&
            ((routeA && adaptiveTimerNonIntegral(&sTimers[ta])) ||
             (routeB && adaptiveTimerNonIntegral(&sTimers[tb]))))
        {
            plan = 0x100 | mode;
            // Each channel is routed either to both outputs or to neither.
            if (ta == tb && ((h >> 8) & 1) == ((h >> 9) & 1) &&
                ((h >> 12) & 1) == ((h >> 13) & 1))
            {
                u32 wa = routeA ? 1u << !!(h & GBA_SOUNDCNT_H_DIRECT_A_VOLUME_FULL) : 0;
                u32 wb = routeB ? 1u << !!(h & GBA_SOUNDCNT_H_DIRECT_B_VOLUME_FULL) : 0;
                plan = 0x200 | mode | (ta << 2) | (wa << 3) | (wb << 5);
            }
            else if (ta == tb)
            {
                u32 va = 1u << !!(h & GBA_SOUNDCNT_H_DIRECT_A_VOLUME_FULL);
                u32 vb = 1u << !!(h & GBA_SOUNDCNT_H_DIRECT_B_VOLUME_FULL);
                u32 al = (h & GBA_SOUNDCNT_H_DIRECT_A_ENABLE_LEFT) ? va : 0;
                u32 ar = (h & GBA_SOUNDCNT_H_DIRECT_A_ENABLE_RIGHT) ? va : 0;
                u32 bl = (h & GBA_SOUNDCNT_H_DIRECT_B_ENABLE_LEFT) ? vb : 0;
                u32 br = (h & GBA_SOUNDCNT_H_DIRECT_B_ENABLE_RIGHT) ? vb : 0;
                plan = 0x40000000u | mode | (ta << 2) | (al << 3) |
                    (ar << 5) | (bl << 7) | (br << 9);
            }
        }
    }
    // Publish all common-path parameters together. No IRQ mask or spin lock.
    sPcmPlan = plan;
}

static inline u32 samplesBeforeOverflow(u32 counter, u32 overflows, u32 mode)
{
    if (!overflows) return 1u << mode;
    return (0x20000u - counter - 1) >> (10 - mode);
}

static inline void mixDirectSamples(int sampA, int sampB, u32 soundCntH,
    s16* outLeft, s16* outRight)
{
    if (soundCntH & GBA_SOUNDCNT_H_DIRECT_A_VOLUME_FULL) sampA *= 2;
    if (soundCntH & GBA_SOUNDCNT_H_DIRECT_B_VOLUME_FULL) sampB *= 2;
    int left = 0, right = 0;
    if (soundCntH & GBA_SOUNDCNT_H_DIRECT_A_ENABLE_LEFT) left += sampA;
    if (soundCntH & GBA_SOUNDCNT_H_DIRECT_A_ENABLE_RIGHT) right += sampA;
    if (soundCntH & GBA_SOUNDCNT_H_DIRECT_B_ENABLE_LEFT) left += sampB;
    if (soundCntH & GBA_SOUNDCNT_H_DIRECT_B_ENABLE_RIGHT) right += sampB;
    *outLeft = clampSample(left);
    *outRight = clampSample(right);
}

void gbas_init(gbas_shared_t* sharedData)
{
    gSoundSharedData = sharedData;
    gbat_initTimer(&sTimers[0]);
    gbat_initTimer(&sTimers[1]);
    gbs_init();
    sPaused = false; //true;
    sSoundBias = 0x0200;
    sPcmPlan = 0;
    sDirectChannels[0].curPlaySampleCount = 0;
    sDirectChannels[0].curPlaySamples = 0;
    sDirectChannels[1].curPlaySampleCount = 0;
    sDirectChannels[1].curPlaySamples = 0;
}

void gbas_setTimerReload(u32 timer, u16 reload)
{
    sTimers[timer].reload = reload;
    refreshAdaptMode();
}

void gbas_setTimerControl(u32 timer, u16 control)
{
    sTimers[timer].control = control;
    refreshAdaptMode();
    if (!(control & GBAT_CONTROL_ENABLED))
    {
        sTimers[timer].isStarted = false;
    }
}

void gbas_setSoundCntHLo(u8 value)
{
    ((u8*)&sSoundCntH)[0] = value;
    refreshAdaptMode();
}

void gbas_setSoundCntHHi(u8 value)
{
    ((u8*)&sSoundCntH)[1] = value & 0x77;
    refreshAdaptMode();
}

static __attribute__((noinline)) void gbas_updateMixerLegacy(s16* outLeft, s16* outRight)
{
    s16 finalLeft = 0;
    s16 finalRight = 0;

    if (!sPaused)
    {
        u32 timer0Overflows = gbat_updateTimer(&sTimers[0]);
        u32 timer1Overflows = gbat_updateTimer(&sTimers[1]);
        
        //master enable
        if (gSoundSharedData->masterEnable)
        {
            u32 soundCntH = sSoundCntH;
            int sampA = updateDirectChannel(&gSoundSharedData->directChannels[0],
                &sDirectChannels[0],
                (soundCntH & GBA_SOUNDCNT_H_DIRECT_A_TIMER_1) ? timer1Overflows : timer0Overflows);
            int sampB = updateDirectChannel(&gSoundSharedData->directChannels[1],
                &sDirectChannels[1],
                (soundCntH & GBA_SOUNDCNT_H_DIRECT_B_TIMER_1) ? timer1Overflows : timer0Overflows);

            if (soundCntH & GBA_SOUNDCNT_H_DIRECT_A_VOLUME_FULL)
                sampA <<= 1;

            int left = 0;
            int right = 0;
            if (soundCntH & GBA_SOUNDCNT_H_DIRECT_A_ENABLE_LEFT)
                left += sampA;
            if (soundCntH & GBA_SOUNDCNT_H_DIRECT_A_ENABLE_RIGHT)
                right += sampA;

            if (soundCntH & GBA_SOUNDCNT_H_DIRECT_B_VOLUME_FULL)
                sampB <<= 1;

            if (soundCntH & GBA_SOUNDCNT_H_DIRECT_B_ENABLE_LEFT)
                left += sampB;
            if (soundCntH & GBA_SOUNDCNT_H_DIRECT_B_ENABLE_RIGHT)
                right += sampB;

            finalLeft = clampSample(left);
            finalRight = clampSample(right);
        }
    }

    *outLeft = finalLeft;
    *outRight = finalRight;
}
static __attribute__((noinline)) void gbas_updateMixerGeneral(s16* outLeft, s16* outRight, u32 plan)
{
    s16 finalLeft = 0, finalRight = 0;
    if (!sPaused)
    {
        // The original timer/FIFO consumer still runs exactly once per output.
        u32 mode = plan & 3;
        u32 before[2] = {0, 0};
        int oldA = 0, oldB = 0;
        if (mode)
        {
            before[0] = sTimers[0].counter;
            before[1] = sTimers[1].counter;
            oldA = (s8)sDirectChannels[0].curPlaySamples;
            oldB = (s8)sDirectChannels[1].curPlaySamples;
        }
        u32 overflows[2] = {gbat_updateTimer(&sTimers[0]), gbat_updateTimer(&sTimers[1])};
        if (gSoundSharedData->masterEnable)
        {
            u32 soundCntH = sSoundCntH;
            u32 ta = !!(soundCntH & GBA_SOUNDCNT_H_DIRECT_A_TIMER_1);
            u32 tb = !!(soundCntH & GBA_SOUNDCNT_H_DIRECT_B_TIMER_1);
            int sampA = updateDirectChannel(&gSoundSharedData->directChannels[0],
                &sDirectChannels[0], overflows[ta]);
            int sampB = updateDirectChannel(&gSoundSharedData->directChannels[1],
                &sDirectChannels[1], overflows[tb]);
            bool routeA = soundCntH & (GBA_SOUNDCNT_H_DIRECT_A_ENABLE_LEFT | GBA_SOUNDCNT_H_DIRECT_A_ENABLE_RIGHT);
            bool routeB = soundCntH & (GBA_SOUNDCNT_H_DIRECT_B_ENABLE_LEFT | GBA_SOUNDCNT_H_DIRECT_B_ENABLE_RIGHT);
            u32 ca = 0, cb = 0;
            if (mode && overflows[ta] <= 1 && overflows[tb] <= 1)
            {
                if (routeA && overflows[ta] && oldA != sampA)
                    ca = samplesBeforeOverflow(before[ta], overflows[ta], mode);
                if (routeB && overflows[tb] && oldB != sampB)
                    cb = samplesBeforeOverflow(before[tb], overflows[tb], mode);
            }
            if (ca || cb)
            {
                u32 steps = 1u << mode;
                int sumLeft = 0, sumRight = 0;
                for (u32 i = 0; i < steps; ++i)
                {
                    s16 left, right;
                    mixDirectSamples(i < ca ? oldA : sampA, i < cb ? oldB : sampB,
                        soundCntH, &left, &right);
                    sumLeft += left;
                    sumRight += right;
                }
                finalLeft = sumLeft >> mode;
                finalRight = sumRight >> mode;
            }
            else
            {
                mixDirectSamples(sampA, sampB, soundCntH, &finalLeft, &finalRight);
            }
        }
    }
    *outLeft = finalLeft;
    *outRight = finalRight;
}


// Rare reconfiguration fallback: the timers have already advanced for this
// output, so consume with the current source selection without advancing twice.
static void finishInstant(u32 soundCntH, u32 n0, u32 n1, s16* outLeft, s16* outRight)
{
    int a = updateDirectChannel(&gSoundSharedData->directChannels[0], &sDirectChannels[0],
        (soundCntH & GBA_SOUNDCNT_H_DIRECT_A_TIMER_1) ? n1 : n0);
    int b = updateDirectChannel(&gSoundSharedData->directChannels[1], &sDirectChannels[1],
        (soundCntH & GBA_SOUNDCNT_H_DIRECT_B_TIMER_1) ? n1 : n0);
    mixDirectSamples(a, b, soundCntH, outLeft, outRight);
}

static __attribute__((noinline)) void gbas_updateMixerCommonMono(s16* outLeft, s16* outRight, u32 plan)
{
    if (sPaused)
    {
        *outLeft = *outRight = 0;
        return;
    }
    u32 index = (plan >> 2) & 1;
    u32 before = sTimers[index].counter;
    u32 n0 = gbat_updateTimer(&sTimers[0]);
    u32 n1 = gbat_updateTimer(&sTimers[1]);
    if (!gSoundSharedData->masterEnable)
    {
        *outLeft = *outRight = 0;
        return;
    }
    u32 n = index ? n1 : n0;
    if (plan != sPcmPlan || n > 1)
    {
        finishInstant(sSoundCntH, n0, n1, outLeft, outRight);
        return;
    }
    int wa = (plan >> 3) & 3, wb = (plan >> 5) & 3;
    if (!n)
    {
        // updateDirectChannel(...,0) only returns this held byte: no FIFO read,
        // cursor update or DMA request is skipped by this replacement.
        int a = (s8)sDirectChannels[0].curPlaySamples;
        int b = (s8)sDirectChannels[1].curPlaySamples;
        *outLeft = *outRight = clampSample(a * wa + b * wb);
        return;
    }
    int oldA = (s8)sDirectChannels[0].curPlaySamples;
    int oldB = (s8)sDirectChannels[1].curPlaySamples;
    int a = updateDirectChannel(&gSoundSharedData->directChannels[0], &sDirectChannels[0], n);
    int b = updateDirectChannel(&gSoundSharedData->directChannels[1], &sDirectChannels[1], n);
    int current = clampSample(a * wa + b * wb);
    u32 mode = plan & 3;
    u32 earlier = (0x20000u - before - 1) >> (10 - mode);
    if (earlier && (oldA != a || oldB != b))
    {
        int previous = clampSample(oldA * wa + oldB * wb);
        // Same timer: both channel changes occur at the same virtual DAC slot.
        // Clip each endpoint first, then weight; this preserves P1 clipping.
        current = (current * (int)((1u << mode) - earlier) + previous * (int)earlier) >> mode;
    }
    *outLeft = *outRight = current;
}


static __attribute__((noinline)) void gbas_updateMixerCommonStereo(s16* outLeft, s16* outRight, u32 plan)
{
    if (sPaused) { *outLeft = *outRight = 0; return; }
    u32 index = (plan >> 2) & 1;
    u32 before = sTimers[index].counter;
    u32 n0 = gbat_updateTimer(&sTimers[0]);
    u32 n1 = gbat_updateTimer(&sTimers[1]);
    if (!gSoundSharedData->masterEnable) { *outLeft = *outRight = 0; return; }
    u32 n = index ? n1 : n0;
    if (plan != sPcmPlan || n > 1)
    {
        finishInstant(sSoundCntH, n0, n1, outLeft, outRight);
        return;
    }
    int al = (plan >> 3) & 3, ar = (plan >> 5) & 3;
    int bl = (plan >> 7) & 3, br = (plan >> 9) & 3;
    int oldA = (s8)sDirectChannels[0].curPlaySamples;
    int oldB = (s8)sDirectChannels[1].curPlaySamples;
    if (!n)
    {
        *outLeft = clampSample(oldA * al + oldB * bl);
        *outRight = clampSample(oldA * ar + oldB * br);
        return;
    }
    int a = updateDirectChannel(&gSoundSharedData->directChannels[0], &sDirectChannels[0], n);
    int b = updateDirectChannel(&gSoundSharedData->directChannels[1], &sDirectChannels[1], n);
    int left = clampSample(a * al + b * bl);
    int right = clampSample(a * ar + b * br);
    u32 mode = plan & 3;
    u32 earlier = (0x20000u - before - 1) >> (10 - mode);
    if (earlier && (oldA != a || oldB != b))
    {
        int oldLeft = clampSample(oldA * al + oldB * bl);
        int oldRight = clampSample(oldA * ar + oldB * br);
        int later = (1u << mode) - earlier;
        left = (left * later + oldLeft * (int)earlier) >> mode;
        right = (right * later + oldRight * (int)earlier) >> mode;
    }
    *outLeft = left;
    *outRight = right;
}

void gbas_updateMixer(s16* outLeft, s16* outRight)
{
    u32 plan = sPcmPlan;
    if (!plan)
        gbas_updateMixerLegacy(outLeft, outRight);
    else if (plan & 0x40000000u)
        gbas_updateMixerCommonStereo(outLeft, outRight, plan);
    else if (plan & 0x200)
        gbas_updateMixerCommonMono(outLeft, outRight, plan);
    else
        gbas_updateMixerGeneral(outLeft, outRight, plan);
}
