#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace session {

class CancellationToken
{
  public:
    void cancel() { cancelled_.store(true, std::memory_order_release); }
    bool isCancelled() const { return cancelled_.load(std::memory_order_acquire); }

  private:
    std::atomic<bool> cancelled_{false};
};

using CancellationTokenPtr = std::shared_ptr<CancellationToken>;

enum class ConcurrencyPolicy { RejectConcurrent, CancelPrevious };
enum class GateResult { Acquired, Rejected, Cancelled };

class IExecutionGate
{
  public:
    virtual ~IExecutionGate() = default;
    virtual GateResult tryAcquire(const std::string& sessionKey,
                                  ConcurrencyPolicy policy,
                                  CancellationTokenPtr& outToken) = 0;
    virtual void release(const std::string& sessionKey) = 0;
    virtual bool isExecuting(const std::string& sessionKey) const = 0;
};

}  // namespace session
