#include "ErrorStatsService.h"
#include <drogon/drogon.h>

namespace metrics {

ErrorStatsService& ErrorStatsService::getInstance() {
    static ErrorStatsService instance;
    return instance;
}

ErrorStatsService::~ErrorStatsService() {
    shutdown();
}

void ErrorStatsService::init(const ErrorStatsConfig& config) {
    LOG_INFO << "[错误统计服务] Service start initialized";

    if (initialized_) {
        LOG_WARN << "[ErrorStats] Service already initialized";
        return;
    }
    
    config_ = config;
    
    if (!config_.enabled) {
        LOG_INFO << "[ErrorStats] Service disabled by config";
        initialized_ = true;
        return;
    }
    
    // 初始化 DB Manager
    dbManager_ = ErrorStatsDbManager::getInstance();
    dbManager_->init();
    
    // 启动后台线程
    running_ = true;
    workerThread_ = std::thread(&ErrorStatsService::workerLoop, this);
    
    // 启动定时清理任务（每小时执行一次）
    drogon::app().getLoop()->runEvery(std::chrono::hours(1), [this]() {
        if (!running_ || !dbManager_) return;
        
        LOG_INFO << "[ErrorStats] Running scheduled cleanup task";
        int totalCleaned = 0;
        
        // 清理过期的明细事件
        if (config_.retentionDaysDetail > 0) {
            int cleaned = dbManager_->cleanupOldEvents(config_.retentionDaysDetail);
            totalCleaned += cleaned;
            LOG_INFO << "[ErrorStats] Cleaned " << cleaned << " old events (retention: "
                     << config_.retentionDaysDetail << " days)";
        }
        
        // 清理过期的聚合数据
        if (config_.retentionDaysAgg > 0) {
            int cleaned = dbManager_->cleanupOldAgg(config_.retentionDaysAgg);
            totalCleaned += cleaned;
            LOG_INFO << "[ErrorStats] Cleaned " << cleaned << " old aggregation records (retention: "
                     << config_.retentionDaysAgg << " days)";
        }
        
        LOG_INFO << "[ErrorStats] Scheduled cleanup completed, total cleaned: " << totalCleaned;
    });
    
    initialized_ = true;
    LOG_INFO << "[错误统计服务] Service initialized, batch_size=" << config_.asyncBatchSize
             << ", flush_ms=" << config_.asyncFlushMs
             << ", retention_detail=" << config_.retentionDaysDetail << "d"
             << ", retention_agg=" << config_.retentionDaysAgg << "d";
}

void ErrorStatsService::shutdown() {
    if (!running_) return;
    
    LOG_INFO << "[ErrorStats] Shutting down...";
    running_ = false;
    cv_.notify_all();
    
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    
    // 最后一次 flush
    flushEvents();
    flushRequestAgg();
    
    LOG_INFO << "[ErrorStats] Shutdown complete, dropped=" << droppedCount_.load();
}

void ErrorStatsService::recordEvent(const ErrorEvent& event) {
    if (!config_.enabled) return;
    
    // 更新 Prometheus 计数器
    updatePrometheusCounters(event);
    
    // 推入队列
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        if (eventQueue_.size() >= MAX_QUEUE_SIZE) {
            // 根据策略丢弃
            if (config_.dropPolicy == DropPolicy::DROP_OLDEST) {
                eventQueue_.pop();
            }
            droppedCount_++;
            
            // 更新 dropped 计数器
            // TODO: Prometheus counter for dropped events
        }
        eventQueue_.push(event);
    }
    
    // 通知 worker
    cv_.notify_one();
}

void ErrorStatsService::recordError(
    Domain domain,
    const std::string& type,
    const std::string& message,
    const std::string& requestId,
    const std::string& provider,
    const std::string& model,
    const std::string& clientType,
    const std::string& apiKind,
    bool stream,
    int httpStatus,
    const Json::Value& detail,
    const std::string& rawSnippet,
    const std::string& toolName
) {
    ErrorEvent event;
    event.ts = std::chrono::system_clock::now();
    event.severity = Severity::ERROR;
    event.domain = domain;
    event.type = type;
    event.message = message;
    event.requestId = requestId;
    event.provider = provider;
    event.model = model;
    event.clientType = clientType;
    event.apiKind = apiKind;
    event.stream = stream;
    event.httpStatus = httpStatus;
    event.detailJson = detail;
    event.rawSnippet = truncateRawSnippet(rawSnippet);
    event.toolName = toolName;
    
    recordEvent(event);
    
    // 同时记录日志
    LOG_ERROR << "[ErrorStats] " << ErrorEvent::domainToString(domain) << "." << type
              << " | req=" << requestId << " | " << message;
}

void ErrorStatsService::recordWarn(
    Domain domain,
    const std::string& type,
    const std::string& message,
    const std::string& requestId,
    const std::string& provider,
    const std::string& model,
    const std::string& clientType,
    const std::string& apiKind,
    bool stream,
    int httpStatus,
    const Json::Value& detail,
    const std::string& rawSnippet,
    const std::string& toolName
) {
    ErrorEvent event;
    event.ts = std::chrono::system_clock::now();
    event.severity = Severity::WARN;
    event.domain = domain;
    event.type = type;
    event.message = message;
    event.requestId = requestId;
    event.provider = provider;
    event.model = model;
    event.clientType = clientType;
    event.apiKind = apiKind;
    event.stream = stream;
    event.httpStatus = httpStatus;
    event.detailJson = detail;
    event.rawSnippet = truncateRawSnippet(rawSnippet);
    event.toolName = toolName;
    
    recordEvent(event);
    
    LOG_WARN << "[ErrorStats] " << ErrorEvent::domainToString(domain) << "." << type
             << " | req=" << requestId << " | " << message;
}

void ErrorStatsService::recordRequestCompleted(const RequestCompletedData& data) {
    if (!config_.enabled || !config_.persistRequestAgg) return;
    
    // 更新 Prometheus
    updatePrometheusRequestCounter(data);
    
    // 推入队列
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        if (requestQueue_.size() >= MAX_QUEUE_SIZE) {
            requestQueue_.pop();
            droppedCount_++;
        }
        requestQueue_.push(data);
    }
    
    cv_.notify_one();
}

void ErrorStatsService::workerLoop() {
    LOG_INFO << "[ErrorStats] Worker thread started";
    
    while (running_) {
        std::unique_lock<std::mutex> lock(eventMutex_);
        
        // 等待条件：队列有数据 或 超时 或 停止
        cv_.wait_for(lock, std::chrono::milliseconds(config_.asyncFlushMs), [this] {
            return !running_ || eventQueue_.size() >= static_cast<size_t>(config_.asyncBatchSize);
        });
        
        lock.unlock();
        
        // 执行 flush
        flushEvents();
        flushRequestAgg();
    }
    
    LOG_INFO << "[ErrorStats] Worker thread exiting";
}

void ErrorStatsService::flushEvents() {
    std::vector<ErrorEvent> batch;
    
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        while (!eventQueue_.empty() && batch.size() < static_cast<size_t>(config_.asyncBatchSize)) {
            batch.push_back(std::move(eventQueue_.front()));
            eventQueue_.pop();
        }
    }
    
    if (batch.empty()) return;
    
    // 写入明细表
    if (config_.persistDetail && dbManager_) {
        if (!dbManager_->insertEvents(batch)) {
            LOG_ERROR << "[ErrorStats] Failed to insert " << batch.size() << " events";
            droppedCount_ += batch.size();
        }
    }
    
    // 更新聚合表
    if (config_.persistAgg && dbManager_) {
        if (!dbManager_->upsertErrorAggHour(batch)) {
            LOG_ERROR << "[ErrorStats] Failed to upsert error aggregation";
        }
    }
}

void ErrorStatsService::flushRequestAgg() {
    std::vector<RequestCompletedData> batch;
    
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        while (!requestQueue_.empty() && batch.size() < static_cast<size_t>(config_.asyncBatchSize)) {
            batch.push_back(std::move(requestQueue_.front()));
            requestQueue_.pop();
        }
    }
    
    if (batch.empty()) return;
    
    if (config_.persistRequestAgg && dbManager_) {
        for (const auto& data : batch) {
            RequestAggData aggData;
            aggData.provider = data.provider;
            aggData.model = data.model;
            aggData.clientType = data.clientType;
            aggData.apiKind = data.apiKind;
            aggData.stream = data.stream;
            aggData.httpStatus = data.httpStatus;
            aggData.ts = data.ts;
            
            if (!dbManager_->upsertRequestAggHour(aggData)) {
                LOG_ERROR << "[ErrorStats] Failed to upsert request aggregation";
            }
        }
    }
}

void ErrorStatsService::flushNow() {
    flushEvents();
    flushRequestAgg();
}

int ErrorStatsService::runCleanup() {
    if (!dbManager_) return 0;
    
    int total = 0;
    total += dbManager_->cleanupOldEvents(config_.retentionDaysDetail);
    total += dbManager_->cleanupOldAgg(config_.retentionDaysAgg);
    
    LOG_INFO << "[ErrorStats] Cleanup completed, removed " << total << " records";
    return total;
}

void ErrorStatsService::updatePrometheusCounters(const ErrorEvent& event) {
    // TODO: 集成 Prometheus 客户端库
    // 示例：使用 drogon 的 PromExporter 或 prometheus-cpp
    // 
    // auto& counter = prometheus::BuildCounter()
    //     .Name("aiapi_error_events_total")
    //     .Help("Total error/warning events")
    //     .Register(*registry);
    // counter.Add({
    //     {"severity", severityToString(event.severity)},
    //     {"domain", domainToString(event.domain)},
    //     {"type", event.type},
    //     {"provider", event.provider},
    //     {"model", event.model},
    //     {"client_type", event.clientType},
    //     {"api_kind", event.apiKind},
    //     {"stream", event.stream ? "true" : "false"}
    // }).Increment();
    
    // 暂时只记录日志
    (void)event; // suppress unused warning
}

void ErrorStatsService::updatePrometheusRequestCounter(const RequestCompletedData& data) {
    // TODO: 集成 Prometheus 客户端库
    // aiapi_requests_total counter
    (void)data; // suppress unused warning
}

std::string ErrorStatsService::truncateRawSnippet(const std::string& snippet) {
    if (!config_.rawSnippetEnabled) {
        return "";
    }
    
    if (snippet.size() <= static_cast<size_t>(config_.rawSnippetMaxLen)) {
        return snippet;
    }
    
    return snippet.substr(0, config_.rawSnippetMaxLen);
}

} // namespace metrics
