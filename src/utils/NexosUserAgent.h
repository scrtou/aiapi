#pragma once

#include <cstdlib>
#include <string>

// The Nexos Cloudflare clearance cookie is bound to the browser User-Agent.
// Keep every C++ request on the same value as the browser/login service.  The
// environment variable is intentionally read once: changing it while the
// process is running cannot make already-issued clearance cookies portable.
namespace nexos {

inline constexpr const char kDefaultUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36";

inline std::string resolveUserAgent(const char* configuredValue)
{
    if (configuredValue == nullptr) {
        return kDefaultUserAgent;
    }

    std::string value(configuredValue);
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return kDefaultUserAgent;
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);

    // Reject embedded line breaks rather than allowing an environment value to
    // turn into multiple HTTP headers.
    if (value.find_first_of("\r\n") != std::string::npos) {
        return kDefaultUserAgent;
    }
    return value;
}

inline const std::string& userAgent()
{
    static const std::string value = resolveUserAgent(std::getenv("NEXOS_USER_AGENT"));
    return value;
}

}  // namespace nexos
