#ifndef CHAYNS_THREAD_REAPER_H
#define CHAYNS_THREAD_REAPER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <platform/ThreadJoin.h>
#include <domain/port/IProviderRegistry.h>

class chaynsThreadDbManager;

/**
 * @brief chayns 上游线程回收器
 *
 * 台账（chaynsa_thread）只负责“记住有哪些上游 thread 还活着”，
 * 真正把它们从 cube.tobit.cloud 删掉是本类的职责。
 *
 * 之所以必须独立成后台线程，而不是挂在会话清理路径里：
 *   会话清理是同步遍历，一轮可能涉及数百个会话；若在其中逐个发上游
 *   HTTP DELETE，清理线程会被网络 IO 卡住数分钟，连带拖垮内存 TTL 精度。
 *   因此请求/清理路径只做台账 detach，删除全部收敛到这里限速执行。
 *
 * 回收判定：last_active_at 早于 (now - idleSeconds) 即视为空闲可回收。
 * 注意这里刻意不区分“已 detach”与“仍绑定但长期无活动”——后者往往是
 * 进程重启后遗留的孤儿行，其内存映射早已不存在，同样必须回收。
 */
class chaynsThreadReaper
{
public:
    struct Options
    {
        int scanIntervalSeconds = 900;   ///< 扫描周期，默认 15 分钟
        int idleSeconds         = 86400; ///< 空闲多久算可回收，默认 24 小时
        int batchLimit          = 50;    ///< 单轮最多处理多少行，避免长时间占用
        int maxAttempts         = 5;     ///< 单行最大删除重试次数，超过按孤儿丢弃
        int deleteSpacingMs     = 200;   ///< 相邻上游删除请求间隔，避免触发风控
    };

    /// Thread ledger is supplied by the composition root.  The reaper never
    /// performs a static store lookup, so its lifetime is explicit and test
    /// fixtures can use an isolated ledger instance.
    explicit chaynsThreadReaper(std::shared_ptr<chaynsThreadDbManager> threadDb);
    ~chaynsThreadReaper();

    /// Composition-root injection; the reaper never performs a static lookup.
    void setProviderRegistry(IProviderRegistry* registry) { providerRegistry_ = registry; }

    /// 幂等：重复调用只会启动一个线程
    void start(const Options& options);

    /// 不限时停机：一直等到 worker 干净退出。析构、测试与运维手动停机走这条。
    void stop();

    /**
     * @brief 带停机预算的停机
     *
     * deadline 是整条停机链共享的绝对时间点（由 AppContext 下发），不是本组件
     * 独享的时长——若各段各自 now()+X，总时长就会累加成 N 倍。
     *
     * 能保证的：睡眠中的 worker 立即被唤醒；逐行删除循环在下一行前退出；
     * 相邻删除之间的限速等待可被打断；停机侧以完成通知和 joinUntil
     * 共享同一个绝对 deadline，不会无限 join。
     * 不能保证的：一次已发出的上游 DELETE 无法撤回，它最长占用
     * chayns::kUpstreamRequestTimeoutSeconds（30s）。预算小于该值时本函数会告警，
     * 让超支在日志里留痕，而不是假装守住了预算。
     *
     * 因此本重载相对 stop() 的实际差异是限时汇合：超预算返回 false、保持线程
     * joinable，调用方可稍后用无参 stop() 安全收割；那条不可撤回的上游 DELETE
     * 仍会按其自身硬上限完成，不会被 detach。
     */
    [[nodiscard]] bool stop(std::chrono::steady_clock::time_point deadline);

    /// 立即执行一轮回收（同步），返回成功删除的上游线程数；供测试与运维触发
    int runOnce();

    Options getOptions() const;

private:
    chaynsThreadReaper(const chaynsThreadReaper&) = delete;
    chaynsThreadReaper& operator=(const chaynsThreadReaper&) = delete;

    void loop();

    /// stop() 与 stop(deadline) 的共同实现；nullopt 表示不限时。
    [[nodiscard]] bool stopInternal(std::optional<std::chrono::steady_clock::time_point> deadline);

    /// 限速等待：可被停机唤醒打断，替代裸 sleep_for。
    void interruptibleSleepFor(std::chrono::milliseconds duration);

    mutable std::mutex      optionsMutex_;
    Options                 options_;
    std::thread             worker_;
    std::mutex              wakeMutex_;
    std::condition_variable wakeCv_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       stopRequested_{false};
    platform::ThreadCompletionPtr workerDone_;
    IProviderRegistry* providerRegistry_ = nullptr;
    std::shared_ptr<chaynsThreadDbManager> threadDb_;
};

#endif  // CHAYNS_THREAD_REAPER_H
