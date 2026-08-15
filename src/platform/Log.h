#pragma once

#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

namespace platform {

/**
 * Small process-local logging facade for application code.
 *
 * Infrastructure and transport may use their framework logger, but application
 * must not import Drogon solely for LOG_* stream macros.  This facade preserves
 * the existing streaming call sites while keeping the dependency at the
 * platform boundary.
 */
class LogLine
{
  public:
    explicit LogLine(const char* level)
    {
        stream_ << '[' << level << "] ";
    }

    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;
    LogLine(LogLine&&) = delete;
    LogLine& operator=(LogLine&&) = delete;

    ~LogLine()
    {
        static std::mutex outputMutex;
        std::lock_guard<std::mutex> lock(outputMutex);
        std::clog << stream_.str() << '\n';
    }

    template <typename Value>
    LogLine& operator<<(Value&& value)
    {
        stream_ << std::forward<Value>(value);
        return *this;
    }

  private:
    std::ostringstream stream_;
};

}  // namespace platform

#ifndef LOG_TRACE
#define LOG_TRACE platform::LogLine("TRACE")
#endif
#ifndef LOG_DEBUG
#define LOG_DEBUG platform::LogLine("DEBUG")
#endif
#ifndef LOG_INFO
#define LOG_INFO platform::LogLine("INFO")
#endif
#ifndef LOG_WARN
#define LOG_WARN platform::LogLine("WARN")
#endif
#ifndef LOG_ERROR
#define LOG_ERROR platform::LogLine("ERROR")
#endif
#ifndef LOG_FATAL
#define LOG_FATAL platform::LogLine("FATAL")
#endif
