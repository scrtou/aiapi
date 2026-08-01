#ifndef CHAYNS_POLLING_POLICY_H
#define CHAYNS_POLLING_POLICY_H

#include <chrono>

namespace chayns {

// A single generation request may spend at most five minutes waiting for the
// asynchronous upstream response. This still accommodates the long-running
// responses observed in production while preventing the old retry-count based
// loop from occupying a worker indefinitely.
constexpr auto kRequestPollingDeadline = std::chrono::minutes(5);

inline std::chrono::milliseconds pollingDelayForElapsed(
    std::chrono::milliseconds elapsed)
{
    if (elapsed < std::chrono::seconds(3)) {
        return std::chrono::milliseconds(200);
    }
    if (elapsed < std::chrono::seconds(15)) {
        return std::chrono::milliseconds(500);
    }
    if (elapsed < std::chrono::seconds(60)) {
        return std::chrono::seconds(1);
    }
    return std::chrono::seconds(2);
}

}  // namespace chayns

#endif
