#pragma once
#include "Fat/ff.h"
#include "ILogger.h"

/// @brief Logs to a FAT file on the SD card (debug builds on real hardware).
///        Enabled by creating an empty /_gba/debug marker file.
class FileLogger : public ILogger
{
    FIL* _file;
    bool _open;
    char _logBuffer[128];

public:
    explicit FileLogger(FIL* file)
        : _file(file), _open(false) { }

    bool Open(const char* path);
    void Close();

    void LogV(LogLevel level, const char* fmt, va_list vlist) override;
};
