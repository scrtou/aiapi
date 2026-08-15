#include <runtime/AppWiring.h>

#include <application/account/accountManager.h>
#include <infrastructure/account/DrogonAccountHttpTransport.h>
#include <infrastructure/account/AccountClock.h>
#include <domain/port/IAccountHttpTransport.h>
#include <infrastructure/account/RetoolProvisionClock.h>
#include <infrastructure/provider/chayns/chaynsapi.h>
#include <infrastructure/provider/chayns/chaynsThreadReaper.h>
#include <infrastructure/provider/retool/retoolapi.h>
#include <application/channel/channelManager.h>
#include <transport/controllers/AiApiController.h>
#include <transport/controllers/AccountController.h>
#include <transport/controllers/HealthController.h>
#include <transport/controllers/ChannelController.h>
#include <transport/controllers/MetricsController.h>
#include <transport/controllers/RetoolWorkspaceController.h>
#include <application/workspace/RetoolWorkspaceUseCase.h>
#include <application/workspace/RetoolWorkspaceAdminUseCase.h>
#include <application/account/AccountAdminUseCase.h>
#include <application/channel/ChannelAdminUseCase.h>
#include <application/health/HealthUseCase.h>
#include <application/metrics/MetricsUseCase.h>
#include <infrastructure/persistence/account/accountDbManager.h>
#include <infrastructure/persistence/account/accountBackupDbManager.h>
#include <infrastructure/persistence/channel/channelDbManager.h>
#include <infrastructure/persistence/chaynsThread/chaynsThreadDbManager.h>
#include <infrastructure/persistence/metrics/ErrorStatsDbManager.h>
#include <infrastructure/persistence/metrics/StatusDbManager.h>
#include <infrastructure/persistence/config/ConfigDbManager.h>
#include <infrastructure/persistence/retoolWorkspace/RetoolWorkspaceDbManager.h>
#include <infrastructure/persistence/session/SessionDbManager.h>
#include <domain/port/IBackgroundExecutor.h>
#include <infrastructure/metrics/ErrorStatsService.h>
#include <infrastructure/provider/ProviderRegistry.h>
#include <infrastructure/provider/ProductionProviderFactory.h>
#include <infrastructure/managedAccount/backends/ClassicProviderAccountBackend.h>
#include <infrastructure/managedAccount/backends/RetoolWorkspaceBackend.h>
#include <infrastructure/managedAccount/service/ManagedAccountService.h>
#include <application/workspace/RetoolWorkspaceManager.h>
#include <infrastructure/workspace/RetoolWorkspaceService.h>
#include <application/generation/continuity/ResponseIndex.h>
#include <application/generation/core/Session.h>
#include <application/generation/core/AiApiUseCase.h>
#include <application/generation/core/SessionExecutionGate.h>
#include <application/generation/core/RequestAdapters.h>
#include <application/generation/tooling/BridgeHelpers.h>
#include <application/generation/core/RetiredProviderTelemetry.h>
#include <infrastructure/executor/BackgroundTaskQueue.h>

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
StartupResult stepSessionTrackingMode(const Json::Value& cfg, AppContext& ctx)
{
    const auto& sessionStore = ctx.sessionStore();
    if (!sessionStore) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "SessionStore is not owned by AppContext");
    }
    std::string mode = "hash";
    if (cfg.isMember("session_tracking")) {
        mode = cfg["session_tracking"].get("mode", "hash").asString();
    }
    if (mode == "zerowidth" || mode == "zero_width") {
        sessionStore->setTrackingMode(SessionTrackingMode::ZeroWidth);
        RequestAdapters::setTrackingMode(SessionTrackingMode::ZeroWidth);
        LOG_INFO << "会话追踪模式：ZeroWidth（零宽字符嵌入）";
    } else {
        sessionStore->setTrackingMode(SessionTrackingMode::Hash);
        RequestAdapters::setTrackingMode(SessionTrackingMode::Hash);
        LOG_INFO << "会话追踪模式：Hash（内容哈希）";
    }
    return StartupResult::ok();
}

// Session/thread persistence are ordinary runtime services, not lazy global
// accessors.  They are created only after the queue is running, so every
// write-through path receives the exact executor whose owner will drain it.
StartupResult stepRuntimeDataServices(AppContext& ctx)
{
    auto* executor = ctx.backgroundExecutor();
    if (!executor) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "BackgroundTaskQueue is not owned by AppContext");
    }

    auto sessionDb = std::make_shared<SessionDbManager>(executor);
    sessionDb->initialize();
    ctx.setSessionPersistence(std::move(sessionDb));

    auto threadDb = std::make_shared<chaynsThreadDbManager>(executor);
    threadDb->initialize();
    ctx.setThreadLedger(std::move(threadDb));

    // Metrics stores are ordinary context-owned infrastructure objects too.
    // They are created before Controller/telemetry wiring, while their table
    // initialization remains inside ErrorStatsService::init() so the existing
    // sink contract has one authoritative lifecycle entry point.
    ctx.setErrorStatsStore(std::make_shared<metrics::ErrorStatsDbManager>());
    ctx.setStatusMetricsStore(std::make_shared<metrics::StatusDbManager>());
    return StartupResult::ok();
}

void addAccountWorkersOwner(AppContext& ctx, std::shared_ptr<AccountManager> accounts)
{
    if (!accounts) {
        return;
    }
    ctx.addOwner("account workers",
                 [accounts](std::chrono::steady_clock::time_point deadline) {
                     // These APIs retain non-owning process-global pointers.
                     // Stop publishing the context-owned account services before
                     // their worker shutdown starts, so no late request can
                     // observe a manager that is draining.
                     RequestAdapters::setAccountSettingsQuery(nullptr);
                     HealthController::setUseCase(nullptr);
                     ChannelController::setUseCase(nullptr);
                     AccountController::setUseCase(nullptr);

                     const auto budget = remainingBudget(deadline);
                     LOG_INFO << "[停机] account workers 剩余预算 "
                              << budget.count() << "ms";

                     // 登录/注册 HTTP 的单次不可撤回上限是 accountManager.cpp
                     // 中的 300 秒。预算更短时只告警，不伪装成可保证的 join。
                     constexpr auto kAccountUpstreamRequestCap =
                         std::chrono::milliseconds(300 * 1000);
                     if (budget < kAccountUpstreamRequestCap) {
                         LOG_WARN << "[停机] account workers 停机预算 " << budget.count()
                                  << "ms 小于单次上游请求上限 "
                                  << kAccountUpstreamRequestCap.count()
                                  << "ms，若此刻正在登录或注册账号，join 可能超出预算";
                     }
                     if (!accounts->stopBackgroundThreads(deadline)) {
                         LOG_WARN << "[停机] account workers 未在预算内退出，线程仍在运行";
                     }
                 });
}

// Store 注入必须早于各自的 init()：init() 会立刻建表/迁移数据。漏注入不会崩溃，
// 只会静默走 Null 实现，所以这一步的存在本身就是那道门禁。
StartupResult stepInjectStores(AppContext& ctx, const Json::Value& runtimeConfig)
{
    auto* executor = ctx.backgroundExecutor();
    if (!executor) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "BackgroundTaskQueue is not available for store injection");
    }

    std::string storeError;
    auto channelStore = std::make_shared<ChannelDbManager>();
    if (!channelStore->initialize(&storeError)) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "ChannelDbManager initialization failed: " + storeError);
    }
    ctx.setChannelStore(channelStore);

    auto channels = std::make_shared<ChannelManager>();
    ctx.setChannelManager(channels);
    channels->setStore(channelStore);
    channels->init();

    // Config persistence is another ordinary context-owned adapter.  Bind it
    // before AccountManager::init(), which may immediately load or persist
    // automation settings through the injected IKeyValueConfigStore port.
    auto configStore = std::make_shared<ConfigDbManager>();
    configStore->initialize();
    ctx.setConfigStore(configStore);

    auto accountStore = std::make_shared<AccountDbManager>();
    if (!accountStore->initialize(&storeError)) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "AccountDbManager initialization failed: " + storeError);
    }
    ctx.setAccountStore(accountStore);

    auto accountBackupStore = std::make_shared<AccountBackupDbManager>();
    if (!accountBackupStore->initialize(&storeError)) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "AccountBackupDbManager initialization failed: " + storeError);
    }
    ctx.setAccountBackupStore(accountBackupStore);

    auto accounts = std::make_shared<AccountManager>();
    // Publish the exact object before init() can start workers.  Later steps
    // receive this same shared object through AppContext; there is no fallback
    // AccountManager singleton to rediscover.
    ctx.setAccountManager(accounts);
    accounts->setStore(accountStore);
    accounts->setChannelStore(channelStore);
    accounts->setHttpTransport(account::makeDrogonAccountHttpTransport());
    accounts->setClock(account::makeRealAccountClock());
    accounts->setRetoolProvisionClock(
        retoolProvision::makeSystemRetoolProvisionClock());
    accounts->setConfigStore(configStore);
    // Capture configuration once at the composition root and pass a value to
    // application workflows.  They must not rediscover Drogon's runtime from
    // a background worker.
    accounts->setRuntimeConfig(runtimeConfig);
    accounts->init();
    // init() has just started the AccountManager workers; register their
    // teardown before any later wiring can fail.
    addAccountWorkersOwner(ctx, accounts);
    RequestAdapters::setAccountSettingsQuery(accounts.get());
    auto accountAdmin = std::make_shared<AccountAdminUseCase>(
        accounts.get(), accounts.get(), accountStore.get(),
        accountBackupStore.get(), channels.get(), executor);
    ctx.setAccountAdminUseCase(accountAdmin);
    AccountController::setUseCase(accountAdmin.get());

    auto channelAdmin = std::make_shared<ChannelAdminUseCase>(
        channels.get(), accounts.get(), executor);
    ctx.setChannelAdminUseCase(channelAdmin);
    ChannelController::setUseCase(channelAdmin.get());

    // This concrete store used to be a lazy process singleton.  Bind it once
    // inside the context instead, before any facade can cause table creation
    // or persistence work.
    auto workspaceStore = std::make_shared<RetoolWorkspaceDbManager>();
    workspaceStore->initialize();
    ctx.setRetoolWorkspaceStore(workspaceStore);
    auto workspaceManager = std::make_shared<RetoolWorkspaceManager>(workspaceStore);
    ctx.setRetoolWorkspaceManager(workspaceManager);
    workspaceManager->init();
    auto workspaceProvisioner = std::make_shared<RetoolWorkspaceService>(workspaceStore);
    ctx.setRetoolWorkspaceProvisioner(workspaceProvisioner);
    auto workspaceUseCase = std::make_shared<workspace::RetoolWorkspaceUseCase>(
        workspaceStore.get(),
        configStore.get(), channels.get());
    ctx.setRetoolWorkspaceUseCase(workspaceUseCase);
    auto workspaceAdmin = std::make_shared<workspace::RetoolWorkspaceAdminUseCase>(
        *workspaceUseCase, *workspaceProvisioner);
    ctx.setRetoolWorkspaceAdminUseCase(workspaceAdmin);
    accounts->setRetoolWorkspaceServices(
        workspaceUseCase.get(), workspaceProvisioner.get());
    RetoolWorkspaceController::setUseCase(workspaceAdmin.get());
    // The controller and AccountManager retain non-owning workspace ports.
    // Register their unpublish action immediately after wiring so startup
    // rollback cannot leave dangling static controller bindings behind.
    ctx.addOwner("workspace service bindings",
                 [accounts](std::chrono::steady_clock::time_point) {
                     RetoolWorkspaceController::setUseCase(nullptr);
                     accounts->setRetoolWorkspaceServices(nullptr, nullptr);
                 });

    return StartupResult::ok();
}

StartupResult stepProviderRegistry(
    AppContext& ctx, std::chrono::steady_clock::time_point processStartTime)
{
    auto registry = std::make_shared<provider::ProviderRegistry>();
    const auto& accounts = ctx.accountManager();
    if (!accounts) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "AccountManager is not owned by AppContext");
    }
    const auto& channels = ctx.channelManager();
    const auto& workspaceUseCase = ctx.retoolWorkspaceUseCase();
    if (!channels || !workspaceUseCase) {
        return StartupResult::failed(
            StartupError::OwnerStartFailed,
            "ChannelManager and RetoolWorkspaceUseCase must be owned by AppContext");
    }
    const auto& threadLedger = ctx.threadLedger();
    if (!threadLedger) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "thread ledger is not owned by AppContext");
    }

    auto classicAccounts = std::make_shared<ClassicProviderAccountBackend>(
        *accounts, *accounts);
    auto retoolAccounts = std::make_shared<RetoolWorkspaceBackend>(
        *workspaceUseCase);
    auto managedAccounts = std::make_shared<ManagedAccountService>(
        std::move(classicAccounts), std::move(retoolAccounts));
    ctx.setManagedAccountService(managedAccounts);

    auto chayns = provider::makeProductionProvider<chaynsapi>(
        *accounts,
        chayns::makeDrogonChaynsHttpTransport(),
        chayns::makeRealChaynsClock(),
        threadLedger);
    const auto chaynsInitialization = chayns->initialize();
    if (!chaynsInitialization) {
        return StartupResult::failed(
            StartupError::OwnerStartFailed,
            "failed to initialize chaynsapi provider: " + chaynsInitialization.error().message);
    }
    if (!registry->registerChatProvider("chaynsapi", chayns, chayns, chayns)) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "failed to register chaynsapi provider");
    }

    auto retoolProvider = provider::makeProductionProvider<retoolapi>(
        retool::makeDrogonRetoolHttpTransport(),
        retool::makeRealRetoolClock(),
        *managedAccounts,
        *workspaceUseCase,
        *channels);
    const auto retoolInitialization = retoolProvider->initialize();
    if (!retoolInitialization) {
        return StartupResult::failed(
            StartupError::OwnerStartFailed,
            "failed to initialize retoolapi provider: " +
                retoolInitialization.error().message);
    }
    if (!registry->registerChatProvider(
            "retoolapi", retoolProvider, retoolProvider, retoolProvider)) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "failed to register retoolapi provider");
    }

    registry->freeze();
    ctx.setProviderRegistry(registry);
    const auto& accountStore = ctx.accountStore();
    if (!accountStore) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "AccountDbManager is not owned by AppContext");
    }
    auto healthUseCase = std::make_shared<HealthUseCase>(
        processStartTime, accountStore, registry.get(), accounts.get());
    ctx.setHealthUseCase(healthUseCase);
    HealthController::setUseCase(healthUseCase.get());
    if (!ctx.sessionStore()) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "SessionStore is not available for ProviderRegistry");
    }
    ctx.sessionStore()->setProviderRegistry(registry.get());
    LOG_INFO << "[ProviderRegistry] 已显式注册 " << registry->providerNames().size()
             << " 个 provider";
    return StartupResult::ok();
}

StartupResult stepSessionServices(AppContext& ctx)
{
    const auto& sessionDb = ctx.sessionPersistence();
    if (!sessionDb) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "SessionDbManager is not owned by AppContext");
    }
    auto responseIndex = std::make_shared<ResponseIndex>(sessionDb.get());
    auto executionGate = std::make_shared<session::SessionExecutionGate>();
    const auto& sessionStore = ctx.sessionStore();
    if (!sessionStore) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "SessionStore is not owned by AppContext");
    }

    ctx.setResponseIndex(responseIndex);
    ctx.setExecutionGate(executionGate);
    sessionStore->setResponseIndex(responseIndex.get());
    sessionStore->setPersistence(sessionDb.get());
    return StartupResult::ok();
}

// The controller gets this one facade rather than six individual runtime
// services.  The facade snapshots those collaborators when a job is admitted,
// so revoking this raw Drogon binding during shutdown cannot race a worker.
StartupResult stepAiApiUseCase(AppContext& ctx, const Json::Value& runtimeConfig)
{
    const auto& registry = ctx.providerRegistry();
    const auto& sessionStore = ctx.sessionStore();
    const auto& responseIndex = ctx.responseIndex();
    const auto& executionGate = ctx.executionGate();
    const auto& channels = ctx.channelManager();
    auto* const executor = ctx.backgroundExecutor();
    if (!registry || !sessionStore || !responseIndex || !executionGate ||
        !channels || !executor) {
        return StartupResult::failed(
            StartupError::OwnerStartFailed,
            "AI API use case requires registry/session/index/gate/channel/executor ownership");
    }

    auto aiApiUseCase = std::make_shared<AiApiUseCase>(
        registry.get(), sessionStore.get(), responseIndex.get(), executionGate.get(),
        channels.get(), executor, runtimeConfig);
    ctx.setAiApiUseCase(aiApiUseCase);
    AiApiController::setUseCase(aiApiUseCase.get());
    // Drogon owns Controller instances.  Revoke this non-owning static pointer
    // immediately after publication so rollback cannot leave it dangling.
    ctx.addOwner("AI API controller binding",
                 [](std::chrono::steady_clock::time_point) {
                     AiApiController::setUseCase(nullptr);
                 });
    return StartupResult::ok();
}

// ErrorStatsService is a normal AppContext-owned worker.  Constructing its
// store, status query adapter and worker here means metrics has no static
// lifetime or lazy singleton fallback left to bypass the shutdown chain.
StartupResult stepErrorStats(const Json::Value& cfg, AppContext& ctx)
{
    const auto& errorStore = ctx.errorStatsStore();
    const auto& statusStore = ctx.statusMetricsStore();
    if (!errorStore || !statusStore) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "metrics stores are not owned by AppContext");
    }

    metrics::ErrorStatsConfig statsConfig;
    if (cfg.isMember("error_stats")) {
        statsConfig = metrics::ErrorStatsConfig::loadFromJson(cfg["error_stats"]);
    }

    auto errorStats = std::make_shared<metrics::ErrorStatsService>(errorStore);
    ctx.setErrorStatsService(errorStats);
    errorStats->init(statsConfig);

    // init() may start a worker.  Register rollback/shutdown ownership before
    // publishing raw telemetry/controller references so a later startup step
    // cannot leave a running worker behind.
    ctx.addOwner("error stats service",
                 [errorStats](std::chrono::steady_clock::time_point deadline) {
                     // These two APIs retain non-owning process-global pointers.
                     // Clear them first so no late request can call into a worker
                     // that is draining or about to be destroyed.
                     bridge::setTelemetrySink(nullptr);
                     observability::setTelemetrySink(nullptr);
                     MetricsController::setUseCase(nullptr);

                     LOG_INFO << "[停机] error stats 剩余预算 "
                              << remainingBudget(deadline).count() << "ms";
                     if (!errorStats->shutdown(deadline)) {
                         LOG_WARN << "[停机] error stats 后台线程未在预算内退出，"
                                  << "尾部落库已跳过";
                     }
                 });

    // StatusDbManager does not start a worker, but explicitly initialize it
    // before publishing the injected query ports to the Controller.
    statusStore->init();
    auto metricsUseCase = std::make_shared<metrics::MetricsUseCase>(
        errorStore.get(), statusStore.get());
    ctx.setMetricsUseCase(metricsUseCase);
    MetricsController::setUseCase(metricsUseCase.get());
    bridge::setTelemetrySink(errorStats.get());
    observability::setTelemetrySink(errorStats.get());
    return StartupResult::ok();
}

// G8 的第一处有意降级：建表失败退回纯内存会话，进程仍可服务。
StartupResult stepSessionPersistence(AppContext& ctx)
{
    auto responseIndex = std::dynamic_pointer_cast<ResponseIndex>(ctx.responseIndex());
    if (!responseIndex) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "ResponseIndex implementation missing during persistence setup");
    }
    const auto& sessionDb = ctx.sessionPersistence();
    if (!sessionDb) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "SessionDbManager is not owned by AppContext");
    }
    std::string dbErr;
    if (sessionDb->ensureTables(&dbErr)) {
        sessionDb->setEnabled(true);
        responseIndex->setPersistenceEnabled(true);
        ctx.sessionStore()->setPersistenceEnabled(true);
        LOG_INFO << "[会话持久化] 已启用：chat_session_state / response_index 写穿与懒加载生效";
        return StartupResult::ok();
    }
    sessionDb->setEnabled(false);
    responseIndex->setPersistenceEnabled(false);
    ctx.sessionStore()->setPersistenceEnabled(false);
    return StartupResult::degraded("会话持久化未启用，降级为纯内存会话: " + dbErr);
}

// G8 的第二处有意降级：台账建不出来时整套回收机制关闭，聊天不受影响。
StartupResult stepchaynsThreadLedger(const Json::Value& cfg, AppContext& ctx)
{
    const auto& threadDb = ctx.threadLedger();
    if (!threadDb) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "chaynsThreadDbManager is not owned by AppContext");
    }
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
    if (!ctx.providerRegistry()) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "ProviderRegistry is not available for thread reaper");
    }
    auto reaper = std::make_shared<chaynsThreadReaper>(threadDb);
    ctx.setThreadReaper(reaper);
    reaper->setProviderRegistry(ctx.providerRegistry().get());
    reaper->start(reaperOpt);
    // 启动后立刻登记：build() 中途失败时，逆序回滚才恰好覆盖「已经起来的那些」。
    ctx.addOwner("chayns thread reaper",
                 [reaper](std::chrono::steady_clock::time_point deadline) {
                     // reaper 已能接收 deadline：睡眠与限速等待均可被打断，
                     // 逐行删除循环也会在预算耗尽时退出。仍不可中断的只剩
                     // 单次已发出的上游 DELETE（30s 硬上限）。
                     // 原 loop() 睡 scanIntervalSeconds（默认 15 分钟），
                     // 停机全靠 wakeCv_ 唤醒。若唤醒丢失，这里的 join 就是 H3 里
                     // 最容易吃满宽限期的一段，故记录剩余预算。
                     LOG_INFO << "[停机] chayns thread reaper 剩余预算 "
                              << remainingBudget(deadline).count() << "ms";
                     if (!reaper->stop(deadline)) {
                         LOG_WARN << "[停机] chayns thread reaper 未在预算内退出，线程仍在运行";
                     }
                 });
    return StartupResult::ok();
}

StartupResult stepSessionTuning(const Json::Value& cfg, AppContext& ctx)
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

    const auto& sessionStore = ctx.sessionStore();
    if (!sessionStore) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "SessionStore is not owned by AppContext");
    }
    auto* cs = sessionStore.get();
    cs->setSessionExpireSeconds(memExpire);
    cs->setCleanupIntervalSeconds(memInterval);
    cs->setDbRetentionSeconds(dbRetention);
    cs->setStoreSessionPayload(storePayload);
    auto responseIndex = std::dynamic_pointer_cast<ResponseIndex>(ctx.responseIndex());
    if (!responseIndex) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "ResponseIndex implementation missing during tuning");
    }
    responseIndex->setStoreResponseBody(storeBody);
    LOG_INFO << "[会话持久化] 参数生效: 内存TTL=" << cs->getSessionExpireSeconds() / 3600.0
             << "h, 内存清理间隔=" << cs->getCleanupIntervalSeconds() / 3600.0
             << "h, DB保留=" << cs->getDbRetentionSeconds() / 3600.0
             << "h, payload落库=" << (cs->isStoreSessionPayloadEnabled() ? "on" : "off")
             << ", response_body落库="
             << (responseIndex->isStoreResponseBodyEnabled() ? "on" : "off");
    return StartupResult::ok();
}

StartupResult stepSessionCleaner(AppContext& ctx)
{
    const auto sessionStore = ctx.sessionStore();
    if (!sessionStore) {
        return StartupResult::failed(StartupError::OwnerStartFailed,
                                     "SessionStore is not owned by AppContext");
    }
    sessionStore->startClearExpiredSession();
    ctx.addOwner("session cleaner",
                 [sessionStore](std::chrono::steady_clock::time_point deadline) {
                     LOG_INFO << "[停机] session cleaner 剩余预算 "
                              << remainingBudget(deadline).count() << "ms";
                     if (!sessionStore->stopClearExpiredSession(deadline)) {
                         LOG_WARN << "[停机] session cleaner 清理线程未在预算内退出，线程仍在运行";
                     }
                 });
    LOG_INFO << "[会话清理] 内存会话过期清理线程已启动";
    return StartupResult::ok();
}

StartupResult stepResponseIndexCleanup(const Json::Value& cfg, AppContext& ctx)
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
        [index = ctx.responseIndex(), maxEntries, maxAgeHours] {
            index->cleanup(static_cast<size_t>(maxEntries), std::chrono::hours(maxAgeHours));
        });
    return StartupResult::ok();
}

}  // namespace

void registerApplicationSteps(
    AppContext& ctx, const Json::Value& customConfig,
    std::chrono::steady_clock::time_point processStartTime)
{
    // The queue itself is the IBackgroundExecutor implementation.  Construct
    // it once at the composition root; no function-static adapter or global
    // queue remains for late callers to rediscover.
    ctx.setBackgroundTaskQueue(std::make_shared<BackgroundTaskQueue>());
    // SessionStore 是有状态且持线程的生命周期服务，由 composition root 唯一持有。
    // 构造本身不启动线程；真正启动仍发生在下方 session cleaner 步骤中。
    ctx.setSessionStore(std::make_shared<chatSession>());
    // 队列先起：后续步骤（账号线程等）会向它投递任务。它同时是最后一个被停的
    // owner——登记顺序即逆序停机顺序，这一点由 AppContext 保证，不再靠注释维持。
    ctx.addStep("background task queue", [&customConfig, &ctx] {
        const int workers = resolveWorkerThreads(customConfig);
        const auto queue = ctx.backgroundTaskQueue();
        if (!queue) {
            return StartupResult::failed(StartupError::OwnerStartFailed,
                                         "BackgroundTaskQueue is not owned by AppContext");
        }
        queue->start(static_cast<size_t>(workers));
        ctx.addOwner("background task queue",
                     [queue](std::chrono::steady_clock::time_point deadline) {
                         // 这是五个 owner 中唯一已具备限时等待能力的一处：
                         // waitUntilIdle(timeout) 只观测不推进状态机，可先按剩余
                         // 预算等待在途任务自然收尾，再进 shutdown() 走 drain+join。
                         //
                         // 顺序不能颠倒：shutdown() 会把状态推到 Draining 并立即
                         // join，一旦进去就再没有「限时」这一说，deadline 也就白拿了。
                         const auto budget = remainingBudget(deadline);
                         if (!queue->waitUntilIdle(budget)) {
                             // 未排空即超预算：如实告警。这条日志是 H3 的直接证据，
                             // 有它才能判断超支是「任务确实没做完」还是「唤醒丢了」。
                             LOG_WARN << "[停机] background task queue 在预算 "
                                      << budget.count() << "ms 内未排空，仍有 "
                                      << queue->runningCount() << " 个任务在执行";
                         }
                         if (!queue->shutdown(deadline)) {
                             LOG_WARN << "[停机] background task queue 工作线程未在预算内退出，"
                                      << "线程仍在运行，等待后续收割";
                         }
                     });
        return StartupResult::ok();
    });

    ctx.addStep("session tracking mode", [&customConfig, &ctx] {
        return stepSessionTrackingMode(customConfig, ctx);
    });
    ctx.addStep("runtime persistence stores", [&ctx] { return stepRuntimeDataServices(ctx); });
    ctx.addStep("inject stores",         [&customConfig, &ctx] {
        return stepInjectStores(ctx, customConfig);
    });
    ctx.addStep("provider registry",     [&ctx, processStartTime] {
        return stepProviderRegistry(ctx, processStartTime);
    });
    ctx.addStep("session services",      [&ctx] { return stepSessionServices(ctx); });
    ctx.addStep("AI API use case",       [&customConfig, &ctx] {
        return stepAiApiUseCase(ctx, customConfig);
    });
    ctx.addStep("error stats",           [&customConfig, &ctx] { return stepErrorStats(customConfig, ctx); });
    ctx.addStep("session persistence",   [&ctx] { return stepSessionPersistence(ctx); });
    ctx.addStep("chayns thread ledger",  [&customConfig, &ctx] { return stepchaynsThreadLedger(customConfig, ctx); });
    ctx.addStep("session tuning",        [&customConfig, &ctx] { return stepSessionTuning(customConfig, ctx); });
    ctx.addStep("session cleaner",       [&ctx] { return stepSessionCleaner(ctx); });
    ctx.addStep("response index cleanup",[&customConfig, &ctx] { return stepResponseIndexCleanup(customConfig, ctx); });
}

}  // namespace lifecycle
