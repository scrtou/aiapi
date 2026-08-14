#pragma once

#include <platform/Cancellation.h>
#include <platform/Deadline.h>

#include <chrono>

namespace provider {

/**
 * Request-scoped controls observed by a provider at every blocking boundary.
 * Event publication joins this context in P6-W2/P7 when the legacy session
 * emission path is replaced; P6-W1 deliberately establishes the immutable
 * cancellation/deadline portion first.
 */
struct ProviderCallContext {
    const platform::CancellationToken& cancellation;
    platform::Deadline deadline;

    [[nodiscard]] bool isCancelled() const { return cancellation.isCancelled(); }
    [[nodiscard]] bool deadlineExceeded() const { return platform::deadlineExpired(deadline); }
    [[nodiscard]] std::chrono::milliseconds remaining() const
    {
        return platform::remainingUntil(deadline);
    }
};

}  // namespace provider
