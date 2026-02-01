#ifndef METRICS_ERROR_STATS_SERVICE_H
#define METRICS_ERROR_STATS_SERVICE_H

#include "ErrorEvent.h"
#include "ErrorStatsConfig.h"
#include "../dbManager/metrics/ErrorStatsDbManager.h"
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <atomic>
#include <chrono>

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
 * @brief 错误统计服务 - 单例
 * 
 * 负责：
 * - 接收错误/警告事件并推入异步队列
 * - 后台线程定期 flush 到数据库
 * - 更新 Prometheus 指标
 * - 管理数据清理任务
 */
class ErrorStatsService {
public:
    static ErrorStatsService& getInstance();
    
    // 禁止拷贝和移动
    ErrorStatsService(const ErrorStatsService&) = delete;
    ErrorStatsService& operator=(const ErrorStatsService&) = delete;
    
    /**
     * @brief 初始化服务
     * @param config 配置
     */
    void init(const ErrorStatsConfig& config);
    
    /**
     * @brief 停止服务并等待后台线程结束
     */
    void shutdown();
    
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
    ErrorStatsService() = default;
    ~ErrorStatsService();
    
    void recordEvent(const ErrorEvent& event);
    void workerLoop();
    void flushEvents();
    void flushRequestAgg();
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
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    
    // 统计
    std::atomic<uint64_t> droppedCount_{0};
    
    // DB Manager
    std::shared_ptr<ErrorStatsDbManager> dbManager_;
    
    // 队列大小上限
    static constexpr size_t MAX_QUEUE_SIZE = 10000;
};

} // namespace metrics

#endif // METRICS_ERROR_STATS_SERVICE_H
