// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/logging.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace bfsr {
namespace {

std::string timestamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto t = clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

} // namespace

const char* toString(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off:   return "OFF";
    }
    return "?";
}

LogLevel logLevelFromString(const std::string& s, LogLevel def) {
    if (s == "trace") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "info")  return LogLevel::Info;
    if (s == "warn" || s == "warning") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    if (s == "off" || s == "none") return LogLevel::Off;
    return def;
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() { shutdown(); }

void Logger::init(const std::string& logFilePath, LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) { std::fclose(static_cast<FILE*>(file_)); file_ = nullptr; }
    level_ = level;
    if (!logFilePath.empty() && level != LogLevel::Off) {
        // Truncate on open: a fresh log per session is far easier to triage than
        // an ever-growing file, and the previous run is preserved as .1 by the
        // installer's rotation step.
        file_ = std::fopen(logFilePath.c_str(), "w");
    }
    initialised_ = true;
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) { std::fflush(static_cast<FILE*>(file_)); std::fclose(static_cast<FILE*>(file_)); file_ = nullptr; }
    initialised_ = false;
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Logger::write(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    writeV(level, fmt, args);
    va_end(args);
}

void Logger::writeV(LogLevel level, const char* fmt, va_list args) {
    if (level == LogLevel::Off || fmt == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level < level_) return;
    }

    std::vector<char> buf(512);
    for (;;) {
        va_list copy;
        va_copy(copy, args);
        const int n = std::vsnprintf(buf.data(), buf.size(), fmt, copy);
        va_end(copy);
        if (n < 0) return;                       // Encoding error; drop the line.
        if (static_cast<size_t>(n) < buf.size()) break;
        if (buf.size() > (1u << 16)) { buf[buf.size() - 1] = '\0'; break; } // Cap runaway lines.
        buf.resize(static_cast<size_t>(n) + 1);
    }

    std::string line = "[" + timestamp() + "][FSR][" + toString(level) + "] " + buf.data();

    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::fputs(line.c_str(), static_cast<FILE*>(file_));
        std::fputc('\n', static_cast<FILE*>(file_));
        // Errors and warnings are exactly the lines that go missing in a crash,
        // so pay the flush cost for them and buffer the rest.
        if (level >= LogLevel::Warn) std::fflush(static_cast<FILE*>(file_));
    }
#if defined(_WIN32)
    OutputDebugStringA((line + "\n").c_str());
#endif
    ring_.push_back(std::move(line));
    while (ring_.size() > ringCapacity_) ring_.pop_front();
}

std::deque<std::string> Logger::recent(size_t maxLines) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.size() <= maxLines) return ring_;
    return std::deque<std::string>(ring_.end() - static_cast<long>(maxLines), ring_.end());
}

void Logger::failureBlock(const std::string& what,
                          const std::string& reason,
                          const std::string& requiredFeature,
                          const std::string& detectedFeature,
                          const std::string& fallback) {
    write(LogLevel::Error, "---- %s failed ----", what.c_str());
    write(LogLevel::Error, "  Reason:            %s", reason.c_str());
    if (!requiredFeature.empty())
        write(LogLevel::Error, "  Required feature:  %s", requiredFeature.c_str());
    if (!detectedFeature.empty())
        write(LogLevel::Error, "  Detected feature:  %s", detectedFeature.c_str());
    write(LogLevel::Error, "  Fallback:          %s", fallback.c_str());
    write(LogLevel::Error, "-------------------------------");
}

} // namespace bfsr
