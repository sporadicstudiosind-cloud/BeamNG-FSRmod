// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <cstdarg>
#include <deque>
#include <mutex>
#include <string>

namespace bfsr {

enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

const char* toString(LogLevel l) noexcept;
LogLevel logLevelFromString(const std::string& s, LogLevel def = LogLevel::Info);

/// Process-wide logger.
///
/// Three consumers, deliberately: a rotating file the user can attach to a bug
/// report, the platform debugger channel, and an in-memory ring the diagnostics
/// UI reads over IPC without touching the disk. Logging must never throw and
/// must never be the reason the host process dies, so every entry point here
/// swallows its own failures.
class Logger {
public:
    static Logger& instance();

    /// Reopens the log file. Safe to call again after a renderer restart.
    void init(const std::string& logFilePath, LogLevel level);
    void shutdown();

    void setLevel(LogLevel level);
    [[nodiscard]] LogLevel level() const;

    void write(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 3, 4)))
#endif
        ;
    void writeV(LogLevel level, const char* fmt, va_list args);

    /// Newest-last snapshot of the ring buffer, for the diagnostics panel.
    [[nodiscard]] std::deque<std::string> recent(size_t maxLines) const;

    /// Emits the standard multi-line failure block described in docs/DIAGNOSTICS.
    void failureBlock(const std::string& what,
                      const std::string& reason,
                      const std::string& requiredFeature,
                      const std::string& detectedFeature,
                      const std::string& fallback);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    mutable std::mutex mutex_;
    void* file_ = nullptr; ///< FILE*, kept opaque so <cstdio> stays out of the header.
    LogLevel level_ = LogLevel::Info;
    std::deque<std::string> ring_;
    size_t ringCapacity_ = 512;
    bool initialised_ = false;
};

} // namespace bfsr

#define BFSR_LOG_TRACE(...) ::bfsr::Logger::instance().write(::bfsr::LogLevel::Trace, __VA_ARGS__)
#define BFSR_LOG_DEBUG(...) ::bfsr::Logger::instance().write(::bfsr::LogLevel::Debug, __VA_ARGS__)
#define BFSR_LOG_INFO(...)  ::bfsr::Logger::instance().write(::bfsr::LogLevel::Info,  __VA_ARGS__)
#define BFSR_LOG_WARN(...)  ::bfsr::Logger::instance().write(::bfsr::LogLevel::Warn,  __VA_ARGS__)
#define BFSR_LOG_ERROR(...) ::bfsr::Logger::instance().write(::bfsr::LogLevel::Error, __VA_ARGS__)
