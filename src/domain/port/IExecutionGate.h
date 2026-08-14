#pragma once

#include <platform/Cancellation.h>

#include <memory>
#include <string>

namespace session {

/**
 * A request gate owns cancellation authority; downstream callers receive only
 * the read-only platform token.  Reusing the platform primitive lets a P6
 * provider observe CancelPrevious without gaining permission to cancel its
 * caller.
 */
using CancellationSourcePtr = std::shared_ptr<platform::CancellationSource>;

enum class ConcurrencyPolicy { RejectConcurrent, CancelPrevious };
enum class GateResult { Acquired, Rejected, Cancelled };

class IExecutionGate
{
  public:
    virtual ~IExecutionGate() = default;
    virtual GateResult tryAcquire(const std::string& sessionKey,
                                  ConcurrencyPolicy policy,
                                  CancellationSourcePtr& outCancellation) = 0;
    /**
     * Release only the lease identified by `cancellation`.
     *
     * CancelPrevious can install a successor before the cancelled request has
     * unwound.  A key-only release would then let that older guard clear the
     * successor's state, so the gate must receive the lease identity back.
     */
    virtual void release(const std::string& sessionKey,
                         const CancellationSourcePtr& cancellation) = 0;
    virtual bool isExecuting(const std::string& sessionKey) const = 0;
};

}  // namespace session
