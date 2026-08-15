#ifndef CHAYNS_POLLING_POLICY_H
#define CHAYNS_POLLING_POLICY_H

#include <chrono>

namespace chayns {

// A single generation request may spend at most five minutes waiting for the
// asynchronous upstream response. This still accommodates the long-running
// responses observed in production while preventing the old retry-count based
// loop from occupying a worker indefinitely.
constexpr auto kRequestPollingDeadline = std::chrono::minutes(5);

// Synchronous upstream calls must carry an explicit timeout.
// kRequestPollingDeadline above only bounds the *loop*. Without a
// per-request timeout a stalled sendRequest never returns, the while
// condition is never re-evaluated, and the 5-minute budget is silently
// bypassed -- the worker stays pinned exactly as the old retry-count
// loop did. These constants close that gap.
// 30.0 follows the convention already used in accountManager.cpp.
constexpr double kUpstreamRequestTimeoutSeconds = 30.0;

// Image upload is the only large-body request. 300.0 mirrors the
// long-upload tier in accountManager.cpp, but is an INFERENCE: no image
// size cap exists in the repo. Revisit once real sizes are measured.
constexpr double kUpstreamUploadTimeoutSeconds = 300.0;

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
