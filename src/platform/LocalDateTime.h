#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace platform {

/** Format the local wall clock in the database's legacy timestamp shape. */
inline std::string localDbTimestampNow()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

/** Parse the legacy local timestamp shape used by account/workspace stores. */
inline std::optional<std::chrono::system_clock::time_point> parseLocalDbTimestamp(
    const std::string& value)
{
    std::tm parsed{};
    std::istringstream input(value);
    input >> std::get_time(&parsed, "%Y-%m-%d %H:%M:%S");
    if (input.fail()) {
        return std::nullopt;
    }
    parsed.tm_isdst = -1;
    const std::time_t timestamp = std::mktime(&parsed);
    if (timestamp == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(timestamp);
}

}  // namespace platform
