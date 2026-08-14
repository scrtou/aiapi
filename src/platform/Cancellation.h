#ifndef AIAPI_PLATFORM_CANCELLATION_H
#define AIAPI_PLATFORM_CANCELLATION_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

/**
 * @brief 停机取消原语（P4-W3 / D2）。
 *
 * 为什么需要它：P4-W2 之后 `AppContext::shutdown(deadline)` 已持有绝对截止时间，
 * 但 `RuntimeOwner::stop` 是 `std::function<void()>`，deadline 进不到 owner 内部。
 * 各 owner 于是各自造了一套停机标志：Reaper 用 `stopRequested_` 原子布尔 +
 * `wakeCv_`，AccountManager 用 `backgroundStopRequested_` + `backgroundWakeCv_`，
 * chatSession 用 `stopClearExpiredLoop_` + `clearExpiredWakeCv_`。三套写法相同、
 * 各自维护同一个「置位必须持锁，否则 notify 落空」的约束——同一个 bug 要修三遍。
 *
 * 与 `SessionExecutionGate` 里的 `CancellationToken` 的关系：那个是**请求级**取消
 * （CancelPrevious 策略下取消上一次生成），生命周期随请求，且没有等待原语。
 * 本类是**进程停机级**取消，附带可中断等待。两者语义不同，不合并，但都由本文件
 * 提供的原语表达，避免第四套实现。
 *
 * 线程模型：`request()` 可由任意线程调用；`waitUntil` / `waitFor` 由被取消的
 * 工作线程调用。置位与谓词读取共用同一个 shared `State::mutex`，杜绝丢失唤醒——这正是
 * Reaper 注释里记录过的那个坑（notify 落空会让停机退化成等满一个扫描周期，
 * 表现为 join 永久挂起）。
 */
namespace platform {

namespace cancellation_detail {

struct State {
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    bool cancelled = false;
};

}  // namespace cancellation_detail

/**
 * Read-only cancellation view for request-scoped consumers.
 *
 * A token deliberately cannot request cancellation: providers may observe a
 * caller's cancellation but cannot cancel another layer's work.  The shared
 * state also keeps a token safe if its source has already been destroyed.
 */
class CancellationToken
{
  public:
    CancellationToken() = default;

    bool isCancelled() const
    {
        if (!state_) return false;
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->cancelled;
    }

    bool waitUntil(std::chrono::steady_clock::time_point deadline) const
    {
        if (!state_) return false;
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->cv.wait_until(lock, deadline, [this]() { return state_->cancelled; });
        return state_->cancelled;
    }

  private:
    explicit CancellationToken(std::shared_ptr<cancellation_detail::State> state)
        : state_(std::move(state))
    {
    }

    std::shared_ptr<cancellation_detail::State> state_;

    friend class CancellationSource;
};

class CancellationSource
{
public:
    CancellationSource()
        : state_(std::make_shared<cancellation_detail::State>())
    {
    }

    CancellationSource(const CancellationSource&)            = delete;
    CancellationSource& operator=(const CancellationSource&) = delete;

    /// 请求取消。幂等：重复调用不改变状态，也不重复唤醒语义。
    void request()
    {
        {
            // 必须持锁置位：等待方的谓词读的是 State::cancelled，锁外置位会与
            // 「谓词已求值、尚未进入 wait」的窗口交错，使 notify 落空。
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->cancelled) {
                return;
            }
            state_->cancelled = true;
        }
        state_->cv.notify_all();
    }

    bool isCancelled() const
    {
        return token().isCancelled();
    }

    /** Return a copyable, read-only token for a provider or other consumer. */
    CancellationToken token() const
    {
        return CancellationToken(state_);
    }

    /**
     * 等到「被取消」或「到达绝对截止时间」为止。
     *
     * @return true 表示因取消而返回；false 表示等到了 deadline。
     *
     * 取绝对时间点而非相对时长：停机宽限期是**跨 owner 共享**的预算，相对时长
     * 会在每个 owner 处重新起算，五个 owner 各睡 2s 就是 10s，宽限期被逐段突破。
     */
    bool waitUntil(std::chrono::steady_clock::time_point deadline) const
    {
        return token().waitUntil(deadline);
    }

    /// 相对时长版本，仅供「本来就是周期轮询、与停机预算无关」的场景使用。
    bool waitFor(std::chrono::milliseconds timeout) const
    {
        return waitUntil(std::chrono::steady_clock::now() + timeout);
    }

private:
    std::shared_ptr<cancellation_detail::State> state_;
};

using CancellationSourcePtr = std::shared_ptr<CancellationSource>;

}  // namespace platform

#endif  // AIAPI_PLATFORM_CANCELLATION_H
