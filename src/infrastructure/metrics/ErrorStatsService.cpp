#include <infrastructure/metrics/ErrorStatsService.h>
#include <infrastructure/metrics/ErrorEventJsonDecoder.h>
#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>

namespace metrics {

ErrorStatsService::ErrorStatsService(std::shared_ptr<IErrorStatsSink> sink)
    : sink_(std::move(sink))
{
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
    
    if (!sink_) {
        // 构造器要求显式 sink；保留这一层运行时防御是为了避免一个错误的
        // nullptr 注入把 eventQueue_ 变成永远无人消费的内存黑洞。
        LOG_ERROR << "[错误统计服务] 未注入 IErrorStatsSink，禁用错误统计";
        config_.enabled = false;
        initialized_ = true;
        return;
    }

    // Metrics 查询端点依赖同一 store。即使事件采集在配置中关闭，也保持
    // 建表/迁移初始化，维持旧接线中 MetricsController 可查询的行为。
    sink_->init();

    if (!config_.enabled) {
        LOG_INFO << "[错误统计服务] 配置已禁用，跳过 worker 启动";
        initialized_ = true;
        return;
    }
    
    // 启动后台线程
    running_ = true;
    workerDone_ = std::make_shared<platform::ThreadCompletion>();
    // 用守卫对象自报完成，而不是在 workerLoop 末尾加一行：后者会被任何提前
    // return 或异常展开绕过，停机侧就要白等满整个预算才发现「没等到」。
    workerThread_ = std::thread([this, done = workerDone_]() {
        struct Signaler {
            platform::ThreadCompletionPtr done;
            ~Signaler() { done->signal(); }
        } signaler{done};
        workerLoop();
    });
    
    // 启动定时清理任务（每小时执行一次）
    // Runtime always creates this service in a shared_ptr owned by AppContext.
    // A weak capture makes the callback harmless for stack-local test fixtures
    // and, more importantly, prevents an EventLoop timer from retaining a raw
    // pointer after AppContext has torn down the service.
    const auto weakSelf = weak_from_this();
    if (!weakSelf.expired()) {
        cleanupLoop_ = drogon::app().getLoop();
        cleanupTimerId_ = cleanupLoop_->runEvery(std::chrono::hours(1), [weakSelf] {
            if (const auto self = weakSelf.lock()) {
                self->runScheduledCleanup();
            }
        });
    }
    
    initialized_ = true;
    LOG_INFO << "[错误统计服务] 初始化完成，batch_size=" << config_.asyncBatchSize
             << ", flush_ms=" << config_.asyncFlushMs
             << ", retention_detail=" << config_.retentionDaysDetail << "d"
             << ", retention_agg=" << config_.retentionDaysAgg << "d";
}

void ErrorStatsService::shutdown() {
    (void)shutdownWithin(nullptr);
}

bool ErrorStatsService::shutdown(std::chrono::steady_clock::time_point deadline) {
    return shutdownWithin(&deadline);
}

bool ErrorStatsService::shutdownWithin(const std::chrono::steady_clock::time_point* deadline) {
    // 幂等：既没在跑、也没有待收割的线程，才算真的无事可做。
    // 注意判据不能只看 running_：上一次限时汇合超预算时会留下一个仍 joinable
    // 的线程，那种状态下必须允许再次进入并把它收割掉。
    if (!running_ && !workerThread_.joinable()) {
        // 配置关闭时没有 worker；仍需让同一个显式对象可在测试或受控
        // 重建场景中再次 init，而不是沿用旧 singleton 的永久空壳状态。
        initialized_ = false;
        return true;
    }

    if (running_) {
        LOG_INFO << "[错误统计服务] 开始关闭服务";
        {
            // 置位必须持 eventMutex_：workerLoop 的 wait_for 谓词读的就是
            // running_，锁外置位会与「谓词已检查、尚未进入等待」的窗口交错，
            // 使 notify 落空。这里最坏情况只多等一个 asyncFlushMs（不会像
            // reaper 那样挂死），但停机延迟不应依赖调度巧合，统一按同一套
            // 约束写，也避免以后把 wait_for 改成 wait 时退化成真死锁。
            std::lock_guard<std::mutex> lock(eventMutex_);
            running_ = false;
        }
        cv_.notify_all();
    }

    // Stop future cleanup callbacks before waiting on the flush worker.  A
    // callback that was already executing keeps a shared self reference via
    // its weak lock, so the store remains valid; cancelling here only prevents
    // new database work from starting after shutdown has been requested.
    if (cleanupLoop_ != nullptr && cleanupTimerId_ != trantor::InvalidTimerId) {
        cleanupLoop_->invalidateTimer(cleanupTimerId_);
        cleanupTimerId_ = trantor::InvalidTimerId;
        cleanupLoop_ = nullptr;
    }

    bool joined = true;
    if (workerThread_.joinable()) {
        if (deadline != nullptr && workerDone_) {
            joined = platform::joinUntil(workerThread_, *workerDone_, *deadline);
        } else {
            // 无预算路径：维持改造前的无限等待语义（析构兜底走这里）。
            workerThread_.join();
        }
    }

    if (!joined) {
        LOG_WARN << "[错误统计服务] 后台线程未在停机预算内退出，未 join、未 detach，"
                 << "线程仍在运行；跳过尾部落库以避免与其并发 flush";
        return false;
    }

    flushEvents();
    flushRequestAgg();

    // 复位 initialized_：改造前它一旦为真永不复位，服务被停过之后 init() 只会
    // 打一条「跳过重复初始化」而不再拉起线程，形成一个看不出来的空壳。
    initialized_ = false;

    LOG_INFO << "[错误统计服务] 关闭完成，累计丢弃事件数=" << droppedCount_.load();
    return true;
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
    event.details = erroreventcodec::detailsFromJson(detail);
    event.rawSnippet = truncateRawSnippet(rawSnippet);
    event.toolName = toolName;
    
    recordEvent(event);
    
    // 同时记录日志
    LOG_ERROR << "[错误统计]" << ErrorEvent::domainToString(domain) << "." << type
              << " | req=" << requestId << " | " << message;
}

void ErrorStatsService::record(const ErrorEvent& event)
{
    auto sanitized = event;
    sanitized.rawSnippet = truncateRawSnippet(sanitized.rawSnippet);
    recordEvent(sanitized);
    if (sanitized.severity == Severity::ERROR) {
        LOG_ERROR << "[错误统计]" << ErrorEvent::domainToString(sanitized.domain)
                  << "." << sanitized.type << " | req=" << sanitized.requestId
                  << " | " << sanitized.message;
    } else {
        LOG_WARN << "[错误统计]" << ErrorEvent::domainToString(sanitized.domain)
                 << "." << sanitized.type << " | req=" << sanitized.requestId
                 << " | " << sanitized.message;
    }
}

void ErrorStatsService::recordRequestCompleted(const RequestCompletedEvent& event)
{
    RequestCompletedData data;
    data.provider = event.provider;
    data.model = event.model;
    data.clientType = event.clientType;
    data.apiKind = event.apiKind;
    data.stream = event.stream;
    data.httpStatus = event.httpStatus;
    data.ts = event.ts;
    recordRequestCompleted(data);
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
    event.details = erroreventcodec::detailsFromJson(detail);
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
    if (config_.persistDetail && sink_) {
        if (!sink_->insertEvents(batch)) {
            LOG_ERROR << "[错误统计] 写入明细事件失败，批大小=" << batch.size();
            droppedCount_ += batch.size();
        }
    }
    
    // 更新聚合表
    if (config_.persistAgg && sink_) {
        if (!sink_->upsertErrorAggHour(batch)) {
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
    
    if (config_.persistRequestAgg && sink_) {
        for (const auto& data : batch) {
            RequestAggData aggData;
            aggData.provider = data.provider;
            aggData.model = data.model;
            aggData.clientType = data.clientType;
            aggData.apiKind = data.apiKind;
            aggData.stream = data.stream;
            aggData.httpStatus = data.httpStatus;
            aggData.ts = data.ts;
            
            if (!sink_->upsertRequestAggHour(aggData)) {
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
    if (!sink_) return 0;
    
    int total = 0;
    total += sink_->cleanupOldEvents(config_.retentionDaysDetail);
    total += sink_->cleanupOldAgg(config_.retentionDaysAgg);
    
    LOG_INFO << "[错误统计] 手动清理完成，总计 " << total << " 条记录";
    return total;
}

void ErrorStatsService::runScheduledCleanup()
{
    if (!running_ || !sink_) return;

    LOG_INFO << "[错误统计] 开始执行定时清理任务";
    const int totalCleaned = runCleanup();
    LOG_INFO << "[错误统计] 清理任务完成，总计清理 " << totalCleaned << " 条记录";
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
