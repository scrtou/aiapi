#pragma once

#include <chrono>

namespace platform {

using Deadline = std::chrono::steady_clock::time_point;

inline bool deadlineExpired(Deadline deadline) noexcept
{
    return std::chrono::steady_clock::now() >= deadline;
}

inline std::chrono::milliseconds remainingUntil(Deadline deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

}  // namespace platform
