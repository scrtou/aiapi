#ifndef CHAYNS_THREAD_REAPER_H
#define CHAYNS_THREAD_REAPER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

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

    static chaynsThreadReaper& getInstance();

    /// 幂等：重复调用只会启动一个线程
    void start(const Options& options);
    void stop();

    /// 立即执行一轮回收（同步），返回成功删除的上游线程数；供测试与运维触发
    int runOnce();

    Options getOptions() const;

private:
    chaynsThreadReaper() = default;
    ~chaynsThreadReaper();
    chaynsThreadReaper(const chaynsThreadReaper&) = delete;
    chaynsThreadReaper& operator=(const chaynsThreadReaper&) = delete;

    void loop();

    mutable std::mutex      optionsMutex_;
    Options                 options_;
    std::thread             worker_;
    std::mutex              wakeMutex_;
    std::condition_variable wakeCv_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       stopRequested_{false};
};

#endif  // CHAYNS_THREAD_REAPER_H
