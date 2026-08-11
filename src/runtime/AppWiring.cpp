#include <runtime/AppWiring.h>

#include <accountManager/accountManager.h>
#include <accountManager/RetoolProvisionClock.h>
#include <apiManager/ApiManager.h>
#include <apipoint/chaynsapi/chaynsThreadReaper.h>
#include <channelManager/channelManager.h>
#include <controllers/HealthController.h>
#include <dbManager/account/accountDbManager.h>
#include <dbManager/channel/channelDbManager.h>
#include <dbManager/chaynsThread/chaynsThreadDbManager.h>
#include <dbManager/metrics/ErrorStatsDbManager.h>
#include <dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h>
#include <dbManager/session/SessionDbManager.h>
#include <metrics/ErrorStatsService.h>
#include <retoolWorkspace/RetoolWorkspaceManager.h>
#include <sessionManager/continuity/ResponseIndex.h>
#include <sessionManager/core/Session.h>
#include <utils/BackgroundTaskQueue.h>

#include <drogon/drogon.h>

#include <algorithm>
#include <string>

namespace lifecycle {

namespace {

/**
 * 把绝对 deadline 换算成「还剩多少」。
 *
 * 每个 owner 的 stop 都要回答同一个问题：我还能等多久？绝对 deadline 减去当下
 * 就是答案，且这个答案在停机链上是逐段递减的——这正是 H3 想要的性质：五个
 * owner 共享一份预算，而不是各自重新起算一份相对超时。
 *
 * 钳到 0 而不放任负值：负值传给 wait_for 是 UB 边缘（实现上多半立即返回，但
 * 语义不可依赖），显式钳零让「已超支」表现为「不再等待」，行为确定。
 */
std::chrono::milliseconds remainingBudget(std::chrono::steady_clock::time_point deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

}  // namespace


int hoursToSeconds(double hours)
{
    const double seconds = hours * 3600.0;
    return seconds < 1.0 ? 1 : static_cast<int>(seconds + 0.5);
}

int resolveWorkerThreads(const Json::Value& customConfig)
{
    int requested = static_cast<int>(BackgroundTaskQueue::kDefaultWorkerThreads);
    if (customConfig.isMember("background_task_threads")) {
        if (customConfig["background_task_threads"].isIntegral()) {
            requested = customConfig["background_task_threads"].asInt();
        } else {
            LOG_WARN << "[后台任务队列] background_task_threads 不是整数，使用默认值 "
                     << BackgroundTaskQueue::kDefaultWorkerThreads;
        }
    }
    const int clamped = std::clamp(requested, 2, 64);
    if (clamped != requested) {
        LOG_WARN << "[后台任务队列] background_task_threads=" << requested
                 << " 超出 2-64 范围，已调整为 " << clamped;
    }
    return clamped;
}

bool resolveThreadLedgerEnabled(const Json::Value& customConfig)
{
    // 未配置时默认开启：关掉回收器意味着上游 thread 只增不减，这种代价不该
    // 由「配置里没写」来隐式决定。
    if (!customConfig.isMember("chayns_thread_reaper") ||
        !customConfig["chayns_thread_reaper"].isObject()) {
        return true;
    }
    return customConfig["chayns_thread_reaper"].get("enabled", true).asBool();
}

namespace {

// 会话追踪模式：与持久化无关，纯内存行为，故无失败路径。
StartupResult stepSessionTrackingMode(const Json::Value& cfg)
{
    std::string mode = "hash";
    if (cfg.isMember("session_tracking")) {
        mode = cfg["session_tracking"].get("mode", "hash").asString();
    }
    if (mode == "zerowidth" || mode == "zero_width") {
        chatSession::getInstance()->setTrackingMode(SessionTrackingMode::ZeroWidth);
        LOG_INFO << "会话追踪模式：ZeroWidth（零宽字符嵌入）";
    } else {
        chatSession::getInstance()->setTrackingMode(SessionTrackingMode::Hash);
        LOG_INFO << "会话追踪模式：Hash（内容哈希）";
    }
    return StartupResult::ok();
}

// Store 注入必须早于各自的 init()：init() 会立刻建表/迁移数据。漏注入不会崩溃，
// 只会静默走 Null 实现，所以这一步的存在本身就是那道门禁。
StartupResult stepInjectStores()
{
    ChannelManager::getInstance().setStore(ChannelDbManager::getInstance());
    ChannelManager::getInstance().init();

    AccountManager::getInstance().setStore(AccountDbManager::getInstance());
    HealthController::setDbProbe(AccountDbManager::getInstance());
    AccountManager::getInstance().setChannelStore(ChannelDbManager::getInstance());
    AccountManager::getInstance().setRetoolProvisionClock(
        retoolProvision::makeSystemRetoolProvisionClock());
    AccountManager::getInstance().init();

    RetoolWorkspaceManager::getInstance().setStore(RetoolWorkspaceDbManager::getInstance());
    RetoolWorkspaceManager::getInstance().init();

    ApiManager::getInstance().init();
    return StartupResult::ok();
}

// G7 第一处收口：init() 内部会拉起 workerThread_，所以本步骤必须自登记 owner。
// 此前它没有登记——停机完全依赖 ~ErrorStatsService() 里的兜底 shutdown()，
// 而单例析构发生在 main 返回之后的静态析构阶段，此时 ErrorStatsDbManager
// 与 drogon 的 DB 客户端可能已先被销毁，shutdown() 尾部的 flushEvents()/
// flushRequestAgg() 就变成对已析构对象的调用；且那一刻已在 deadline 之外，
// 停机日志也不会提到它。登记为 owner 后，join 与最后一次 flush 都被拉回
// shutdown(deadline) 的确定性时序内。
StartupResult stepErrorStats(AppContext& ctx)
{
    metrics::ErrorStatsService::getInstance().setSink(
        metrics::ErrorStatsDbManager::getInstance());
    metrics::ErrorStatsConfig statsConfig;
    metrics::ErrorStatsService::getInstance().init(statsConfig);
    // 只在真起了线程时登记：配置禁用时 init() 直接返回，没有可停的东西，
    // 登记一个实际空转的 stop 只会让停机日志谎报「已停错误统计服务」。
    if (statsConfig.enabled) {
        ctx.addOwner("error stats service",
                     [](std::chrono::steady_clock::time_point deadline) {
                         // shutdown() 尾部还要 flushEvents()/flushRequestAgg() 落库，
                         // 是整条停机链上唯一会碰 DB 的一段。剩余预算先记下来：
                         // D4 给 shutdown 加 deadline 形参后，这里直接透传即可。
                         LOG_INFO << "[停机] error stats 剩余预算 "
                                  << remainingBudget(deadline).count() << "ms";
                         if (!metrics::ErrorStatsService::getInstance().shutdown(deadline)) {
                             LOG_WARN << "[停机] error stats 后台线程未在预算内退出，"
                                      << "尾部落库已跳过";
                         }
                     });
    }
    return StartupResult::ok();
}

// G8 的第一处有意降级：建表失败退回纯内存会话，进程仍可服务。
StartupResult stepSessionPersistence()
{
    auto sessionDb = SessionDbManager::getInstance();
    std::string dbErr;
    if (sessionDb->ensureTables(&dbErr)) {
        sessionDb->setEnabled(true);
        ResponseIndex::instance().setPersistenceEnabled(true);
        chatSession::getInstance()->setPersistenceEnabled(true);
        LOG_INFO << "[会话持久化] 已启用：chat_session_state / response_index 写穿与懒加载生效";
        return StartupResult::ok();
    }
    sessionDb->setEnabled(false);
    ResponseIndex::instance().setPersistenceEnabled(false);
    chatSession::getInstance()->setPersistenceEnabled(false);
    return StartupResult::degraded("会话持久化未启用，降级为纯内存会话: " + dbErr);
}

// G8 的第二处有意降级：台账建不出来时整套回收机制关闭，聊天不受影响。
StartupResult stepchaynsThreadLedger(const Json::Value& cfg, AppContext& ctx)
{
    auto threadDb = chaynsThreadDbManager::getInstance();
    if (!resolveThreadLedgerEnabled(cfg)) {
        threadDb->setEnabled(false);
        return StartupResult::degraded("chayns 线程台账已按配置关闭，上游 thread 不再回收");
    }

    std::string threadDbErr;
    if (!threadDb->ensureTable(&threadDbErr)) {
        threadDb->setEnabled(false);
        return StartupResult::degraded("chayns 线程台账未启用，上游 thread 不会被回收: " + threadDbErr);
    }

    threadDb->setEnabled(true);
    LOG_INFO << "[chayns线程台账] 已启用：chaynsa_thread 写穿生效";

    chaynsThreadReaper::Options reaperOpt;
    if (cfg.isMember("chayns_thread_reaper") && cfg["chayns_thread_reaper"].isObject()) {
        const Json::Value& rc = cfg["chayns_thread_reaper"];
        reaperOpt.scanIntervalSeconds = static_cast<int>(rc.get("scan_interval_minutes", 15).asDouble() * 60);
        reaperOpt.idleSeconds         = static_cast<int>(rc.get("idle_hours", 24).asDouble() * 3600);
        reaperOpt.batchLimit          = rc.get("batch_limit", 50).asInt();
        reaperOpt.maxAttempts         = rc.get("max_attempts", 5).asInt();
        reaperOpt.deleteSpacingMs     = rc.get("delete_spacing_ms", 200).asInt();
    }
    chaynsThreadReaper::getInstance().start(reaperOpt);
    // 启动后立刻登记：build() 中途失败时，逆序回滚才恰好覆盖「已经起来的那些」。
    ctx.addOwner("chayns thread reaper",
                 [](std::chrono::steady_clock::time_point deadline) {
                     // reaper 已能接收 deadline：睡眠与限速等待均可被打断，
                     // 逐行删除循环也会在预算耗尽时退出。仍不可中断的只剩
                     // 单次已发出的上游 DELETE（30s 硬上限）。
                     // 原 loop() 睡 scanIntervalSeconds（默认 15 分钟），
                     // 停机全靠 wakeCv_ 唤醒。若唤醒丢失，这里的 join 就是 H3 里
                     // 最容易吃满宽限期的一段，故记录剩余预算。
                     LOG_INFO << "[停机] chayns thread reaper 剩余预算 "
                              << remainingBudget(deadline).count() << "ms";
                     chaynsThreadReaper::getInstance().stop(deadline);
                 });
    return StartupResult::ok();
}

StartupResult stepSessionTuning(const Json::Value& cfg)
{
    int  memExpire    = SESSION_EXPIRE_TIME;
    int  memInterval  = SESSION_CLEANUP_INTERVAL;
    int  dbRetention  = SESSION_EXPIRE_TIME;
    bool storePayload = true;
    bool storeBody    = false;

    if (cfg.isMember("session_persistence") && cfg["session_persistence"].isObject()) {
        const auto& sp = cfg["session_persistence"];
        if (sp["memory_expire_hours"].isNumeric())           memExpire   = hoursToSeconds(sp["memory_expire_hours"].asDouble());
        if (sp["memory_cleanup_interval_hours"].isNumeric()) memInterval = hoursToSeconds(sp["memory_cleanup_interval_hours"].asDouble());
        if (sp["db_retention_hours"].isNumeric())            dbRetention = hoursToSeconds(sp["db_retention_hours"].asDouble());
        storePayload = sp.get("store_session_payload", storePayload).asBool();
        storeBody    = sp.get("store_response_body", storeBody).asBool();
    }

    auto* cs = chatSession::getInstance();
    cs->setSessionExpireSeconds(memExpire);
    cs->setCleanupIntervalSeconds(memInterval);
    cs->setDbRetentionSeconds(dbRetention);
    cs->setStoreSessionPayload(storePayload);
    ResponseIndex::instance().setStoreResponseBody(storeBody);
    LOG_INFO << "[会话持久化] 参数生效: 内存TTL=" << cs->getSessionExpireSeconds() / 3600.0
             << "h, 内存清理间隔=" << cs->getCleanupIntervalSeconds() / 3600.0
             << "h, DB保留=" << cs->getDbRetentionSeconds() / 3600.0
             << "h, payload落库=" << (cs->isStoreSessionPayloadEnabled() ? "on" : "off")
             << ", response_body落库="
             << (ResponseIndex::instance().isStoreResponseBodyEnabled() ? "on" : "off");
    return StartupResult::ok();
}

StartupResult stepSessionCleaner(AppContext& ctx)
{
    chatSession::getInstance()->startClearExpiredSession();
    ctx.addOwner("session cleaner",
                 [](std::chrono::steady_clock::time_point deadline) {
                     LOG_INFO << "[停机] session cleaner 剩余预算 "
                              << remainingBudget(deadline).count() << "ms";
                     chatSession::getInstance()->stopClearExpiredSession();
                 });
    LOG_INFO << "[会话清理] 内存会话过期清理线程已启动";
    return StartupResult::ok();
}

StartupResult stepResponseIndexCleanup(const Json::Value& cfg)
{
    int maxEntries     = 200000;
    int maxAgeHours    = 6;
    int cleanupMinutes = 10;
    if (cfg.isMember("response_index") && cfg["response_index"].isObject()) {
        maxEntries     = cfg["response_index"].get("max_entries", maxEntries).asInt();
        maxAgeHours    = cfg["response_index"].get("max_age_hours", maxAgeHours).asInt();
        cleanupMinutes = cfg["response_index"].get("cleanup_interval_minutes", cleanupMinutes).asInt();
    }
    if (maxEntries <= 0 || maxAgeHours <= 0 || cleanupMinutes <= 0) {
        return StartupResult::degraded("response_index 定时清理已按配置关闭，索引将无限增长");
    }
    drogon::app().getLoop()->runEvery(
        static_cast<double>(cleanupMinutes * 60),
        [maxEntries, maxAgeHours] {
            ResponseIndex::instance().cleanup(static_cast<size_t>(maxEntries),
                                              std::chrono::hours(maxAgeHours));
        });
    return StartupResult::ok();
}

}  // namespace

void registerApplicationSteps(AppContext& ctx, const Json::Value& customConfig)
{
    // 队列先起：后续步骤（账号线程等）会向它投递任务。它同时是最后一个被停的
    // owner——登记顺序即逆序停机顺序，这一点由 AppContext 保证，不再靠注释维持。
    ctx.addStep("background task queue", [&customConfig, &ctx] {
        const int workers = resolveWorkerThreads(customConfig);
        BackgroundTaskQueue::instance().start(static_cast<size_t>(workers));
        ctx.addOwner("background task queue",
                     [](std::chrono::steady_clock::time_point deadline) {
                         // 这是五个 owner 中唯一已具备限时等待能力的一处：
                         // waitUntilIdle(timeout) 只观测不推进状态机，可先按剩余
                         // 预算等待在途任务自然收尾，再进 shutdown() 走 drain+join。
                         //
                         // 顺序不能颠倒：shutdown() 会把状态推到 Draining 并立即
                         // join，一旦进去就再没有「限时」这一说，deadline 也就白拿了。
                         auto& queue = BackgroundTaskQueue::instance();
                         const auto budget = remainingBudget(deadline);
                         if (!queue.waitUntilIdle(budget)) {
                             // 未排空即超预算：如实告警。这条日志是 H3 的直接证据，
                             // 有它才能判断超支是「任务确实没做完」还是「唤醒丢了」。
                             LOG_WARN << "[停机] background task queue 在预算 "
                                      << budget.count() << "ms 内未排空，仍有 "
                                      << queue.runningCount() << " 个任务在执行";
                         }
                         queue.shutdown();
                     });
        return StartupResult::ok();
    });

    ctx.addStep("session tracking mode", [&customConfig] { return stepSessionTrackingMode(customConfig); });
    ctx.addStep("inject stores",         [] { return stepInjectStores(); });
    // 账号后台线程由 AccountManager::init() 内部拉起（见 stepInjectStores），
    // 故其停机动作在同一批次登记，位置严格晚于队列、早于会话设施。
    ctx.addStep("account workers owner", [&ctx] {
        ctx.addOwner("account workers",
                     [](std::chrono::steady_clock::time_point deadline) {
                         // 四个巡检线程的可中断睡眠最长 5 小时，join 前必须依赖
                         // 两组 notify 全部生效；漏掉任一组就会挂满宽限期。
                         const auto budget = remainingBudget(deadline);
                         LOG_INFO << "[停机] account workers 剩余预算 "
                                  << budget.count() << "ms";

                         // D11：可中断化之后，账号线程的停机耗时上界不再是
                         // isServerReachable 的 300 次重试（最坏约 7.6 小时），而是
                         // 一次「已经发出、无法撤回」的上游请求。该请求的超时实参写死在
                         // accountManager.cpp 的登录与注册两处 sendHttpRequest 里，均为
                         // 300.0 秒——阈值即取自那两个字面量，不是本处另拍的数。
                         //
                         // 这里刻意不复用 chayns::kUpstreamRequestTimeoutSeconds（30s）：
                         // 那是 chayns 上游的口径，与账号登录/注册不是同一条链路，混用会让
                         // 告警阈值与它声称的来源脱钩。两者是否应该统一，见待议项 D11-Q1。
                         constexpr auto kAccountUpstreamRequestCap =
                             std::chrono::milliseconds(300 * 1000);
                         if (budget < kAccountUpstreamRequestCap) {
                             // 如实告警而不是静默：预算比单次请求上限还短时，join 就是
                             // 可能超支的那一段。有这条日志才能把「超支」与「唤醒丢了」区分开。
                             LOG_WARN << "[停机] account workers 停机预算 " << budget.count()
                                      << "ms 小于单次上游请求上限 "
                                      << kAccountUpstreamRequestCap.count()
                                      << "ms，若此刻正在登录或注册账号，join 可能超出预算";
                         }
                         AccountManager::getInstance().stopBackgroundThreads();
                     });
        return StartupResult::ok();
    });
    ctx.addStep("error stats",           [&ctx] { return stepErrorStats(ctx); });
    ctx.addStep("session persistence",   [] { return stepSessionPersistence(); });
    ctx.addStep("chayns thread ledger",  [&customConfig, &ctx] { return stepchaynsThreadLedger(customConfig, ctx); });
    ctx.addStep("session tuning",        [&customConfig] { return stepSessionTuning(customConfig); });
    ctx.addStep("session cleaner",       [&ctx] { return stepSessionCleaner(ctx); });
    ctx.addStep("response index cleanup",[&customConfig] { return stepResponseIndexCleanup(customConfig); });
}

}  // namespace lifecycle
