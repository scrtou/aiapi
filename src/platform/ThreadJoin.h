#ifndef AIAPI_PLATFORM_THREAD_JOIN_H
#define AIAPI_PLATFORM_THREAD_JOIN_H

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include <platform/Cancellation.h>

/**
 * @brief 带截止时间的线程汇合（P4-W3 / D3，消解缺口 H5）。
 *
 * 问题：`std::thread::join()` 没有限时版本。一旦调用就只能等到线程真正退出，
 * 停机宽限期对它完全无效——这是 D1 记录的 H5：五处 join 全部无超时上限，
 * 任一 worker 卡住（例如卡在一次已发出、最长 30s 的上游 DELETE 上），
 * 整条停机链就跟着卡满。deadline 传得再准，到 join 这一步也白拿。
 *
 * 解法：让线程**自报完成**，停机侧限时等这个「已完成」事实，而不是限时 join。
 * worker 退出前调用 `Completion::signal()`；停机侧 `waitUntil(deadline)`：
 *   - 在预算内等到完成 → join() 此时必然立即返回，安全汇合；
 *   - 到 deadline 仍未完成 → **不调用 join()**，如实返回 false 交由调用方留痕。
 *
 * 为什么不 detach()：detach 能让停机不挂死，但线程仍在访问它捕获的对象，
 * 与本工作项的退出门禁「deadline 超时路径不得析构仍被活动线程访问的对象」
 * 直接冲突。超时后是「记录 + 继续持有」还是「升级为硬退出」属于 D5 的处置决策，
 * 本原语只负责把「超预算」这一事实变成可观测的返回值，不替 D5 做决定。
 *
 * 为什么复用 CancellationSource 而不自建 mutex+cv+flag：D2 已把
 * 「置位必须持锁、否则 notify 落空」这一约束收敛到一处。本类若另起一套，
 * 就是同一个坑的第五个实例——D2 的落点说明里明确警告过这一点。
 * 语义映射是精确的：`request()` = 线程已完成，`waitUntil()` = 限时等该事实。
 */
namespace platform {

class ThreadCompletion
{
public:
    /// 由工作线程在**即将退出前**调用。幂等，可重复调用。
    void signal() { source_.request(); }

    /// 线程是否已自报完成。
    bool isComplete() const { return source_.isCancelled(); }

    /**
     * 等到线程自报完成，或到达绝对截止时间。
     * @return true 已完成；false 到 deadline 仍未完成。
     */
    bool waitUntil(std::chrono::steady_clock::time_point deadline) const
    {
        return source_.waitUntil(deadline);
    }

private:
    CancellationSource source_;
};

using ThreadCompletionPtr = std::shared_ptr<ThreadCompletion>;

/**
 * 在 deadline 之前汇合 `thread`。
 *
 * @param thread     待汇合的线程；未 joinable 时视为已汇合，返回 true。
 * @param completion 该线程持有的完成标志（须由线程退出前 signal）。
 * @param deadline   绝对截止时间，与 `AppContext::shutdown(deadline)` 同一份预算。
 * @return true  线程已在预算内完成并已 join；
 *         false 预算耗尽仍未完成，**未 join**，线程仍在运行。
 *
 * 返回 false 时线程对象保持 joinable：调用方必须自行决定后续处置（留痕、
 * 二次等待、或按 D5 的结论升级）。此处刻意不 detach、不 terminate。
 */
inline bool joinUntil(std::thread&           thread,
                      const ThreadCompletion& completion,
                      std::chrono::steady_clock::time_point deadline)
{
    if (!thread.joinable()) {
        return true;
    }
    if (!completion.waitUntil(deadline)) {
        return false;
    }
    // 已自报完成：线程正处于「退出前最后几条指令」到「已退出」之间，
    // join 至多阻塞这一小段，不会吃掉剩余预算。
    thread.join();
    return true;
}

}  // namespace platform

#endif  // AIAPI_PLATFORM_THREAD_JOIN_H
