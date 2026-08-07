#include "common.h"
#include <string.h>
#include "mini-printf.h"
#include "Cpsr.h"
#include "FileLogger.h"

[[gnu::section(".ewram")]]
bool FileLogger::Open(const char* path)
{
    if (f_open(_file, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return false;
    _open = true;
    return true;
}

[[gnu::section(".ewram")]]
void FileLogger::Close()
{
    if (_open)
    {
        f_close(_file);
        _open = false;
    }
}

[[gnu::section(".ewram")]]
void FileLogger::LogV(LogLevel level, const char* fmt, va_list vlist)
{
    (void)level;
    if (!_open)
        return;

    u32 irqs = arm_disableIrqs();
    {
        mini_vsnprintf(_logBuffer, sizeof(_logBuffer), fmt, vlist);
        UINT bw;
        if (f_write(_file, _logBuffer, strlen(_logBuffer), &bw) == FR_OK)
        {
            // Keep the log observable even after a power loss / crash.
            f_sync(_file);
        }
    }
    arm_restoreIrqs(irqs);
}
