#pragma once

#include <iostream>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
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
    // Configure an application log sink independent of the process launcher.
    // This keeps application diagnostics available when aiapi is started
    // directly inside Docker (without restart_aiapi.sh redirecting stdout).
    static void configureFile(const std::string& path)
    {
        std::lock_guard<std::mutex> lock(outputMutex_());
        auto& file = file_();
        file.close();
        if (path.empty()) return;
        file.open(path, std::ios::out | std::ios::app);
    }

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
        std::lock_guard<std::mutex> lock(outputMutex_());
        const std::string line = stream_.str();
        std::clog << line << '\n';
        auto& file = file_();
        if (file.is_open()) {
            file << line << '\n';
            file.flush();
        }
    }

    template <typename Value>
    LogLine& operator<<(Value&& value)
    {
        stream_ << std::forward<Value>(value);
        return *this;
    }

  private:
    static std::mutex& outputMutex_()
    {
        static std::mutex mutex;
        return mutex;
    }

    static std::ofstream& file_()
    {
        static std::ofstream file;
        return file;
    }

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
