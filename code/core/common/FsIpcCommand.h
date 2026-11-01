#pragma once

typedef enum
{
    FS_IPC_CMD_DLDI_SETUP,
    FS_IPC_CMD_DLDI_READ_SECTORS,
    FS_IPC_CMD_DLDI_WRITE_SECTORS,
    FS_IPC_CMD_DSI_SD_READ_SECTORS,
    FS_IPC_CMD_DSI_SD_WRITE_SECTORS
} FsIpcCommand;

typedef enum { FS_READ_PENDING = 0, FS_READ_OK = 1, FS_READ_FAILED = 2 } FsReadResult;

typedef struct __attribute__((aligned(32)))
{
    u32 cmd;
    void* buffer;
    u32 sector;
    u32 count;
    vu32 readResult; // Existing padding; ARM9 invalidates this line after ACK.
} fs_ipc_cmd_t;

#ifdef __cplusplus
static_assert(sizeof(fs_ipc_cmd_t) == 32);
static_assert(__builtin_offsetof(fs_ipc_cmd_t, readResult) == 16);
#else
_Static_assert(sizeof(fs_ipc_cmd_t) == 32, "IPC packet size");
_Static_assert(__builtin_offsetof(fs_ipc_cmd_t, readResult) == 16, "IPC result offset");
#endif
