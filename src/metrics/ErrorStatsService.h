#ifndef METRICS_ERROR_STATS_SERVICE_H
#define METRICS_ERROR_STATS_SERVICE_H

#include <domain/model/ErrorEvent.h>
#include <json/json.h>
#include <metrics/ErrorStatsConfig.h>
#include <domain/model/RequestAggData.h>
#include <domain/port/IErrorStatsSink.h>
#include <domain/port/ITelemetrySink.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <platform/ThreadJoin.h>

namespace trantor {
class EventLoop;
}

namespace metrics {

/**
 * @brief 请求完成数据，用于聚合请求总数
 */
struct RequestCompletedData {
    std::string provider;
    std::string model;
    std::string clientType;
    std::string apiKind;
    bool stream = false;
    int httpStatus = 200;
    std::chrono::system_clock::time_point ts = std::chrono::system_clock::now();
};

/**
 * @brief 错误统计服务
 * 
 * 负责：
 * - 接收错误/警告事件并推入异步队列
 * - 后台线程定期 flush 到数据库
 * - 更新 Prometheus 指标
 * - 管理数据清理任务
 */
class ErrorStatsService : public ITelemetrySink,
                          public std::enable_shared_from_this<ErrorStatsService> {
public:
    /**
     * @brief 构造一个显式拥有的错误统计服务。
     *
     * 落库端口是必需的 composition-root 依赖。这里没有默认 DB singleton
     * fallback：漏接线必须在构造点可见，而不是等到 worker 首次 flush 时
     * 才悄悄重新定位一个全局对象。
     */
    explicit ErrorStatsService(std::shared_ptr<IErrorStatsSink> sink);
    ~ErrorStatsService();

    // 禁止拷贝和移动
    ErrorStatsService(const ErrorStatsService&) = delete;
    ErrorStatsService& operator=(const ErrorStatsService&) = delete;

    /**
     * @brief 初始化服务
     * @param config 配置
     */
    void init(const ErrorStatsConfig& config);
    
    /**
     * @brief 停止服务并【无上限】等待后台线程结束。
     *
     * 保留此重载的原因有二：~ErrorStatsService() 的兜底停机没有预算可用；
     * 既有调用方（测试等）依赖它。语义与改造前完全一致。
     */
    void shutdown();

    /**
     * @brief 停止服务，并在 deadline 之前汇合后台线程（P4-W3 / D4，J5）。
     *
     * @return true  线程已在预算内退出，尾部 flushEvents()/flushRequestAgg()
     *               已执行，服务已复位为可重新 init 的状态；
     *         false 预算耗尽，线程仍在运行且**未 detach**。此时刻意
     *               **不做**尾部落库、也不复位 initialized_：那个线程仍在
     *               访问本对象的队列，再从停机侧并发 flush 只会制造竞争；
     *               复位 initialized_ 更会允许再起第二个 worker。
     *               调用方可在稍后用无参 shutdown() 重新收割该线程。
     */
    bool shutdown(std::chrono::steady_clock::time_point deadline);

    /**
     * @brief 是否【尚未请求停机】。
     *
     * 注意这不是「线程是否存活」：shutdownWithin() 在 joinUntil() 之前就把
     * running_ 置 false，因此限时停机超预算返回 false 时，本函数已经报 false，
     * 而那条线程仍在运行。要判断「是否还有线程待收割」，用 hasPendingWorker()。
     *
     * 用途：确认 init() 这一轮是否真的拉起了 worker（撞上未复位的 initialized_
     * 会变成 no-op），否则后续断言会被空壳状态免费满足。
     */
    bool isRunning() const { return running_.load(); }

    /**
     * @brief 是否仍有未被 join 的后台线程（限时停机超预算后的残局标志）。
     *
     * 超预算路径刻意不 detach、不清理，线程仍归本对象所有；此时
     * isRunning()==false 但本函数为 true。调用方据此决定是否再用无参
     * shutdown() 收割一次。线程收割干净后本函数转为 false 且不再回真。
     */
    bool hasPendingWorker() const { return workerThread_.joinable(); }
    
    /**
     * @brief 记录错误事件
     */
    void recordError(
        Domain domain,
        const std::string& type,
        const std::string& message,
        const std::string& requestId = "",
        const std::string& provider = "",
        const std::string& model = "",
        const std::string& clientType = "",
        const std::string& apiKind = "",
        bool stream = false,
        int httpStatus = 0,
        const Json::Value& detail = Json::Value(),
        const std::string& rawSnippet = "",
        const std::string& toolName = ""
    );
    
    /**
     * @brief 记录警告事件
     */
    void recordWarn(
        Domain domain,
        const std::string& type,
        const std::string& message,
        const std::string& requestId = "",
        const std::string& provider = "",
        const std::string& model = "",
        const std::string& clientType = "",
        const std::string& apiKind = "",
        bool stream = false,
        int httpStatus = 0,
        const Json::Value& detail = Json::Value(),
        const std::string& rawSnippet = "",
        const std::string& toolName = ""
    );
    
    /**
     * @brief 记录请求完成（用于请求总数统计）
     */
    void recordRequestCompleted(const RequestCompletedData& data);
    void record(const ErrorEvent& event) override;
    void recordRequestCompleted(const RequestCompletedEvent& event) override;
    
    /**
     * @brief 获取丢弃的事件计数
     */
    uint64_t getDroppedCount() const { return droppedCount_.load(); }
    
    /**
     * @brief 立即 flush 队列到数据库（用于测试或优雅关闭）
     */
    void flushNow();
    
    /**
     * @brief 执行数据清理
     * @return 清理的记录数
     */
    int runCleanup();
    
private:
    void recordEvent(const ErrorEvent& event);
    void workerLoop();
    bool shutdownWithin(const std::chrono::steady_clock::time_point* deadline);
    void flushEvents();
    void flushRequestAgg();
    void runScheduledCleanup();
    void updatePrometheusCounters(const ErrorEvent& event);
    void updatePrometheusRequestCounter(const RequestCompletedData& data);
    std::string truncateRawSnippet(const std::string& snippet);
    
    // 配置
    ErrorStatsConfig config_;
    bool initialized_ = false;
    
    // 事件队列
    std::queue<ErrorEvent> eventQueue_;
    std::mutex eventMutex_;
    
    // 请求聚合队列
    std::queue<RequestCompletedData> requestQueue_;
    std::mutex requestMutex_;
    
    // 后台线程
    std::thread workerThread_;
    // 每次 init() 重建：ThreadCompletion 是一次性的（signal 后永久为真）。
    // 若复用同一个实例，第二次停机会看到「上一个线程留下的已完成」而立刻
    // join 一个仍在运行的新线程 —— 限时汇合会静默退化成无限阻塞。
    platform::ThreadCompletionPtr workerDone_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};

    // 定时清理与服务本体同属 AppContext 生命周期。timer callback 捕获
    // weak_from_this()，shutdown 后先注销 timer，避免 context 销毁后保留
    // 一个悬垂的 [this] 回调。
    trantor::EventLoop* cleanupLoop_ = nullptr;
    std::uint64_t cleanupTimerId_ = 0;
    
    // 统计
    std::atomic<uint64_t> droppedCount_{0};
    

    std::shared_ptr<IErrorStatsSink> sink_;
    
    // 队列大小上限
    static constexpr size_t MAX_QUEUE_SIZE = 10000;
};

} // 命名空间结束

#endif // 头文件保护结束
