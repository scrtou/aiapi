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
    LOG_INFO << "[错误统计服务] 开始";

    if (initialized_) {
        LOG_WARN << "[错误统计服务] 已初始化，跳过重复初始化";
        return;
    }
    
    config_ = config;
    
    if (!config_.enabled) {
        LOG_INFO << "[错误统计服务] 配置已禁用，跳过初始化";
        initialized_ = true;
        return;
    }
    

    dbManager_ = ErrorStatsDbManager::getInstance();
    dbManager_->init();
    
    // 启动后台线程
    running_ = true;
    workerThread_ = std::thread(&ErrorStatsService::workerLoop, this);
    
    // 启动定时清理任务（每小时执行一次）
    drogon::app().getLoop()->runEvery(std::chrono::hours(1), [this]() {
        if (!running_ || !dbManager_) return;
        
        LOG_INFO << "[错误统计] 开始执行定时清理任务";
        int totalCleaned = 0;
        
        // 清理过期的明细事件
        if (config_.retentionDaysDetail > 0) {
            int cleaned = dbManager_->cleanupOldEvents(config_.retentionDaysDetail);
            totalCleaned += cleaned;
            LOG_INFO << "[错误统计] 已清理明细事件 " << cleaned << " 条（保留 "
                     << config_.retentionDaysDetail << " 天）";
        }
        
        // 清理过期的聚合数据
        if (config_.retentionDaysAgg > 0) {
            int cleaned = dbManager_->cleanupOldAgg(config_.retentionDaysAgg);
            totalCleaned += cleaned;
            LOG_INFO << "[错误统计] 已清理聚合数据 " << cleaned << " 条（保留 "
                     << config_.retentionDaysAgg << " 天）";
        }
        
        LOG_INFO << "[错误统计] 清理任务完成，总计清理 " << totalCleaned << " 条记录";
    });
    
    initialized_ = true;
    LOG_INFO << "[错误统计服务] 初始化完成，batch_size=" << config_.asyncBatchSize
             << ", flush_ms=" << config_.asyncFlushMs
             << ", retention_detail=" << config_.retentionDaysDetail << "d"
             << ", retention_agg=" << config_.retentionDaysAgg << "d";
}

void ErrorStatsService::shutdown() {
    if (!running_) return;
    
    LOG_INFO << "[错误统计服务] 开始关闭服务";
    running_ = false;
    cv_.notify_all();
    
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    

    flushEvents();
    flushRequestAgg();
    
    LOG_INFO << "[错误统计服务] 关闭完成，累计丢弃事件数=" << droppedCount_.load();
}

void ErrorStatsService::recordEvent(const ErrorEvent& event) {
    if (!config_.enabled) return;
    

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
            


        }
        eventQueue_.push(event);
    }
    
    // 通知 工作线程
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
    LOG_ERROR << "[错误统计]" << ErrorEvent::domainToString(domain) << "." << type
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
    
    LOG_WARN << "[错误统计]" << ErrorEvent::domainToString(domain) << "." << type
             << " | req=" << requestId << " | " << message;
}

void ErrorStatsService::recordRequestCompleted(const RequestCompletedData& data) {
    if (!config_.enabled || !config_.persistRequestAgg) return;
    

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
    LOG_INFO << "[错误统计服务] 后台工作线程已启动";
    
    while (running_) {
        std::unique_lock<std::mutex> lock(eventMutex_);
        
        // 等待条件：队列有数据 或 超时 或 停止
        cv_.wait_for(lock, std::chrono::milliseconds(config_.asyncFlushMs), [this] {
            return !running_ || eventQueue_.size() >= static_cast<size_t>(config_.asyncBatchSize);
        });
        
        lock.unlock();
        

        flushEvents();
        flushRequestAgg();
    }
    
    LOG_INFO << "[错误统计服务] 后台工作线程已退出";
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
            LOG_ERROR << "[错误统计] 写入明细事件失败，批大小=" << batch.size();
            droppedCount_ += batch.size();
        }
    }
    
    // 更新聚合表
    if (config_.persistAgg && dbManager_) {
        if (!dbManager_->upsertErrorAggHour(batch)) {
            LOG_ERROR << "[错误统计] 写入错误聚合数据失败";
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
                LOG_ERROR << "[错误统计] 写入请求聚合数据失败";
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
    
    LOG_INFO << "[错误统计] 手动清理完成，总计 " << total << " 条记录";
    return total;
}

void ErrorStatsService::updatePrometheusCounters(const ErrorEvent& event) {
    // 待办：接入 Prometheus 客户端库
    // 示例：可接入 Drogon PromExporter 或自定义指标上报
    // 










    // 可在此补充客户端类型维度（clientType）的指标标签



    
    // 暂时只记录日志
    (void)event; // 显式忽略未使用告警，避免编译器噪声
}

void ErrorStatsService::updatePrometheusRequestCounter(const RequestCompletedData& data) {
    // 待办：接入 Prometheus 客户端库

    (void)data; // 显式忽略未使用告警，避免编译器噪声
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

} // 命名空间结束
