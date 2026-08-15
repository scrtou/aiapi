#include <drogon/drogon_test.h>

#include <runtime/AppContext.h>
#include <runtime/StartupResult.h>
#include <application/account/accountManager.h>
#include <application/account/AccountAdminUseCase.h>
#include <application/channel/ChannelAdminUseCase.h>
#include <application/health/HealthUseCase.h>
#include <application/metrics/MetricsUseCase.h>
#include <application/workspace/RetoolWorkspaceAdminUseCase.h>
#include <application/workspace/RetoolWorkspaceUseCase.h>
#include <application/channel/channelManager.h>
#include <infrastructure/persistence/account/accountBackupDbManager.h>
#include <infrastructure/persistence/account/accountDbManager.h>
#include <infrastructure/persistence/channel/channelDbManager.h>
#include <infrastructure/persistence/config/ConfigDbManager.h>
#include <infrastructure/persistence/metrics/ErrorStatsDbManager.h>
#include <infrastructure/persistence/retoolWorkspace/RetoolWorkspaceDbManager.h>
#include <infrastructure/persistence/metrics/StatusDbManager.h>
#include <infrastructure/persistence/session/SessionDbManager.h>
#include <domain/port/IBackgroundExecutor.h>
#include <infrastructure/metrics/ErrorStatsService.h>
#include <application/workspace/RetoolWorkspaceManager.h>
#include <infrastructure/executor/BackgroundTaskQueue.h>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

using lifecycle::AppContext;
using lifecycle::StartupError;
using lifecycle::StartupResult;

namespace {

std::chrono::steady_clock::time_point soon()
{
    return std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

class RecordingExecutor final : public IBackgroundExecutor
{
  public:
    TaskSubmitResult submit(const std::string& name, std::function<void()>) override
    {
        names.push_back(name);
        return result;
    }

    TaskSubmitResult result = TaskSubmitResult::Accepted;
    std::vector<std::string> names;
};

class TestWorkspaceProvisioner final : public workspace::IRetoolWorkspaceProvisioner
{
  public:
    RetoolWorkspaceInfo provision(const std::string&) override { return {}; }
};

class TestAiApiUseCase final : public aiapi::IAiApiUseCase
{
  public:
    aiapi::SubmissionResult submitGeneration(
        aiapi::GenerationInput, SinkFactory, Completion) override
    {
        return {};
    }
    aiapi::ModelCatalogResult modelCatalog(const std::string&) const override { return {}; }
    aiapi::StoredResponseResult getResponse(const std::string&) override { return {}; }
    aiapi::DeleteResponseResult deleteResponse(const std::string&) override { return {}; }
};

}  // namespace

/*
 * P5-W3: BackgroundTaskQueue is no longer a process singleton or a
 * function-static adapter dependency.  The same object AppContext owns must
 * be what application code receives through IBackgroundExecutor.
 */
DROGON_TEST(AppContextPublishesItsOwnedQueueAsBackgroundExecutor)
{
    AppContext ctx;
    auto queue = std::make_shared<BackgroundTaskQueue>();
    ctx.setBackgroundTaskQueue(queue);

    REQUIRE(ctx.backgroundTaskQueue() == queue);
    REQUIRE(ctx.backgroundExecutor() == static_cast<IBackgroundExecutor*>(queue.get()));

    queue->start(1);
    std::atomic<int> executed{0};
    CHECK(ctx.backgroundExecutor()->submit("context-owned-executor", [&executed] {
              ++executed;
          }) == TaskSubmitResult::Accepted);
    CHECK(queue->waitUntilIdle(std::chrono::seconds(5)) == true);
    CHECK(executed.load() == 1);
    queue->shutdown();
}

/*
 * The session persistence write-through path used to rediscover both its own
 * singleton and BackgroundTaskQueue::instance().  A local manager now only
 * submits to the executor passed by its owner; this fake observes all five
 * request-path write kinds without touching a drogon DB client.
 */
DROGON_TEST(SessionDbManagerAsyncWritesUseInjectedExecutorOnly)
{
    RecordingExecutor executor;
    auto persistence = std::make_shared<SessionDbManager>(&executor);
    persistence->setEnabled(true);

    SessionDbManager::SessionRow session;
    session.sessionId = "session-id";
    SessionDbManager::ResponseRow response;
    response.responseId = "response-id";

    persistence->asyncUpsertSession(session);
    persistence->asyncUpsertResponse(response);
    persistence->asyncDeleteSession("session-id");
    persistence->asyncDeleteSessions({"session-id"});
    persistence->asyncDeleteResponses({"response-id"});

    const std::vector<std::string> expected{
        "session.upsert", "responseIndex.upsert", "session.delete",
        "session.deleteBatch", "responseIndex.deleteBatch"};
    CHECK(executor.names == expected);
}

/*
 * Metrics used to be split across three unrelated process singletons.  The
 * AppContext now keeps the worker and both concrete query/store adapters in
 * one lifetime graph, so the worker's injected sink cannot outlive its store.
 */
DROGON_TEST(AppContextOwnsMetricsWorkerAndStores)
{
    AppContext ctx;
    auto errors = std::make_shared<metrics::ErrorStatsDbManager>();
    auto status = std::make_shared<metrics::StatusDbManager>();
    auto service = std::make_shared<metrics::ErrorStatsService>(errors);

    ctx.setErrorStatsStore(errors);
    ctx.setStatusMetricsStore(status);
    ctx.setErrorStatsService(service);

    CHECK(ctx.errorStatsStore() == errors);
    CHECK(ctx.statusMetricsStore() == status);
    CHECK(ctx.errorStatsService() == service);
}

/*
 * P5-W3: account selection, channel catalog and every controller-facing
 * application facade are ordinary context-owned objects.  The test
 * deliberately drops all local owners after publishing them, proving
 * AppContext is the remaining lifetime root rather than a function-static
 * helper in AppWiring.
 */
DROGON_TEST(AppContextOwnsAccountChannelAndApplicationFacades)
{
    AppContext ctx;
    auto channels = std::make_shared<ChannelManager>();
    auto workspaces = std::make_shared<workspace::RetoolWorkspaceUseCase>(
        nullptr, nullptr, channels.get());
    auto accounts = std::make_shared<AccountManager>();
    auto admin = std::make_shared<AccountAdminUseCase>(
        accounts.get(), accounts.get(), nullptr, nullptr, channels.get(), nullptr);
    auto channelAdmin = std::make_shared<ChannelAdminUseCase>(
        channels.get(), accounts.get(), nullptr);
    auto health = std::make_shared<HealthUseCase>(
        std::chrono::steady_clock::now(), nullptr, nullptr, accounts.get());
    auto metrics = std::make_shared<metrics::MetricsUseCase>(nullptr, nullptr);
    auto provisioner = std::make_shared<TestWorkspaceProvisioner>();
    auto workspaceAdmin = std::make_shared<workspace::RetoolWorkspaceAdminUseCase>(
        *workspaces, *provisioner);

    ctx.setChannelManager(channels);
    ctx.setRetoolWorkspaceUseCase(workspaces);
    ctx.setAccountManager(accounts);
    ctx.setAccountAdminUseCase(admin);
    ctx.setChannelAdminUseCase(channelAdmin);
    ctx.setHealthUseCase(health);
    ctx.setMetricsUseCase(metrics);
    ctx.setRetoolWorkspaceProvisioner(provisioner);
    ctx.setRetoolWorkspaceAdminUseCase(workspaceAdmin);

    channels.reset();
    workspaces.reset();
    accounts.reset();
    admin.reset();
    channelAdmin.reset();
    health.reset();
    metrics.reset();
    provisioner.reset();
    workspaceAdmin.reset();

    REQUIRE(ctx.channelManager() != nullptr);
    REQUIRE(ctx.retoolWorkspaceUseCase() != nullptr);
    REQUIRE(ctx.accountManager() != nullptr);
    REQUIRE(ctx.accountAdminUseCase() != nullptr);
    REQUIRE(ctx.channelAdminUseCase() != nullptr);
    REQUIRE(ctx.healthUseCase() != nullptr);
    REQUIRE(ctx.metricsUseCase() != nullptr);
    REQUIRE(ctx.retoolWorkspaceProvisioner() != nullptr);
    REQUIRE(ctx.retoolWorkspaceAdminUseCase() != nullptr);
}

DROGON_TEST(AppContextOwnsAiApiUseCase)
{
    AppContext ctx;
    auto facade = std::make_shared<TestAiApiUseCase>();
    const auto* expected = facade.get();

    ctx.setAiApiUseCase(facade);
    facade.reset();

    REQUIRE(ctx.aiApiUseCase() != nullptr);
    CHECK(ctx.aiApiUseCase().get() == expected);
}

/*
 * P5-W3 workspace lifecycle increment: both the legacy store facade and the
 * provisioner port are published by AppContext.  The local owners are dropped
 * deliberately so a future function-static accessor cannot silently become
 * their actual lifetime root again.
 */
DROGON_TEST(AppContextOwnsRetoolWorkspaceManagerAndProvisioner)
{
    AppContext ctx;
    auto manager = std::make_shared<RetoolWorkspaceManager>(
        std::shared_ptr<IRetoolWorkspaceStore>{});
    auto provisioner = std::make_shared<TestWorkspaceProvisioner>();

    ctx.setRetoolWorkspaceManager(manager);
    ctx.setRetoolWorkspaceProvisioner(provisioner);

    manager.reset();
    provisioner.reset();

    REQUIRE(ctx.retoolWorkspaceManager() != nullptr);
    REQUIRE(ctx.retoolWorkspaceProvisioner() != nullptr);
}

/*
 * The concrete workspace adapter is also context-owned.  It has no lazy
 * process-global accessor now, so dropping the local owner must leave the
 * exact same store reachable only through AppContext's lifecycle graph.
 */
DROGON_TEST(AppContextOwnsRetoolWorkspaceConcreteStore)
{
    AppContext ctx;
    auto store = std::make_shared<RetoolWorkspaceDbManager>();
    const auto* expected = store.get();

    ctx.setRetoolWorkspaceStore(store);
    store.reset();

    REQUIRE(ctx.retoolWorkspaceStore() != nullptr);
    CHECK(ctx.retoolWorkspaceStore().get() == expected);
}

/*
 * AccountManager and the workspace application facade both borrow this config
 * port.  Keeping its concrete adapter under AppContext prevents either path
 * from recreating a ConfigDbManager singleton after a partial migration.
 */
DROGON_TEST(AppContextOwnsConcreteConfigStore)
{
    AppContext ctx;
    auto store = std::make_shared<ConfigDbManager>();
    const auto* expected = store.get();

    ctx.setConfigStore(store);
    store.reset();

    REQUIRE(ctx.configStore() != nullptr);
    CHECK(ctx.configStore().get() == expected);
}

/*
 * The remaining account/channel persistence adapters share the same lifecycle
 * rule: no static accessor can outlive AppContext or bypass its startup graph.
 */
DROGON_TEST(AppContextOwnsConcreteAccountAndChannelStores)
{
    AppContext ctx;
    auto accounts = std::make_shared<AccountDbManager>();
    auto backups = std::make_shared<AccountBackupDbManager>();
    auto channels = std::make_shared<ChannelDbManager>();
    const auto* expectedAccounts = accounts.get();
    const auto* expectedBackups = backups.get();
    const auto* expectedChannels = channels.get();

    ctx.setAccountStore(accounts);
    ctx.setAccountBackupStore(backups);
    ctx.setChannelStore(channels);
    accounts.reset();
    backups.reset();
    channels.reset();

    REQUIRE(ctx.accountStore() != nullptr);
    REQUIRE(ctx.accountBackupStore() != nullptr);
    REQUIRE(ctx.channelStore() != nullptr);
    CHECK(ctx.accountStore().get() == expectedAccounts);
    CHECK(ctx.accountBackupStore().get() == expectedBackups);
    CHECK(ctx.channelStore().get() == expectedChannels);
}

DROGON_TEST(AppContextRunsStepsInRegistrationOrder)
{
    AppContext ctx;
    std::vector<std::string> trace;

    ctx.addStep("first", [&] { trace.push_back("first"); return StartupResult::ok(); });
    ctx.addStep("second", [&] { trace.push_back("second"); return StartupResult::ok(); });
    ctx.addStep("third", [&] { trace.push_back("third"); return StartupResult::ok(); });

    const auto r = ctx.build();
    CHECK(r.isOk());
    CHECK(ctx.isBuilt());
    CHECK(ctx.stepsCompleted() == 3);
    REQUIRE(trace.size() == 3);
    CHECK(trace[0] == "first");
    CHECK(trace[1] == "second");
    CHECK(trace[2] == "third");
}

/**
 * G5 的核心断言：第 3 步失败时，前两步登记的 owner 必须按**逆序**停掉，
 * 且第 4 步不得执行。此前 main.cc 没有任何回滚路径——失败即带着半启动状态继续。
 */
DROGON_TEST(AppContextRollsBackStartedOwnersInReverseOnFailure)
{
    AppContext ctx;
    std::vector<std::string> trace;

    ctx.addStep("start-A", [&] {
        trace.push_back("start-A");
        ctx.addOwner("A", [&](std::chrono::steady_clock::time_point) { trace.push_back("stop-A"); });
        return StartupResult::ok();
    });
    ctx.addStep("start-B", [&] {
        trace.push_back("start-B");
        ctx.addOwner("B", [&](std::chrono::steady_clock::time_point) { trace.push_back("stop-B"); });
        return StartupResult::ok();
    });
    ctx.addStep("boom", [&] {
        trace.push_back("boom");
        return StartupResult::failed(StartupError::StoreInitFailed, "account store missing");
    });
    ctx.addStep("never", [&] {
        trace.push_back("never");
        return StartupResult::ok();
    });

    const auto r = ctx.build();
    CHECK(r.isFailed());
    CHECK(!r.canProceed());
    CHECK(r.error() == StartupError::StoreInitFailed);
    CHECK(!ctx.isBuilt());
    CHECK(ctx.stepsCompleted() == 2);

    const std::vector<std::string> expected{
        "start-A", "start-B", "boom", "stop-B", "stop-A"};
    CHECK(trace == expected);
}

/**
 * G1：执行器拒收初始化任务此前只打日志就继续跑。现在必须是终止性失败，
 * 且原因码可从结果读出，不必翻日志正文。
 */
DROGON_TEST(AppContextExecutorRejectedTerminatesBuild)
{
    AppContext ctx;
    bool laterRan = false;

    ctx.addStep("enqueue-init", [] {
        return StartupResult::failed(StartupError::ExecutorRejected,
                                     "queueInLoop rejected init task");
    });
    ctx.addStep("later", [&] { laterRan = true; return StartupResult::ok(); });

    const auto r = ctx.build();
    CHECK(r.isFailed());
    CHECK(r.error() == StartupError::ExecutorRejected);
    CHECK(!laterRan);
    CHECK(ctx.stepsCompleted() == 0);
}

/**
 * G8：会话快照表 / chayns 台账建表失败是有意降级，进程必须继续启动完成，
 * 且降级原因要能被启动日志与后续 /ready 检索到。
 */
DROGON_TEST(AppContextDegradedStepsDoNotStopBuildButAreRecorded)
{
    AppContext ctx;
    bool tailRan = false;

    ctx.addStep("session-snapshot-table",
                [] { return StartupResult::degraded("table unavailable, memory-only sessions"); });
    ctx.addStep("chayns-thread-ledger",
                [] { return StartupResult::degraded("ledger unavailable, reaper disabled"); });
    ctx.addStep("tail", [&] { tailRan = true; return StartupResult::ok(); });

    const auto r = ctx.build();
    CHECK(r.isDegraded());
    CHECK(r.canProceed());
    CHECK(tailRan);
    CHECK(ctx.isBuilt());
    CHECK(ctx.stepsCompleted() == 3);
    REQUIRE(ctx.degradedReasons().size() == 2);
    CHECK(ctx.degradedReasons()[0].find("session-snapshot-table") != std::string::npos);
    CHECK(ctx.degradedReasons()[1].find("chayns-thread-ledger") != std::string::npos);
}

/// 步骤未填函数体属装配错误，必须失败而非静默跳过（否则重现 G3 类隐式顺序破坏）。
DROGON_TEST(AppContextNullStepIsFailureNotSkip)
{
    AppContext ctx;
    ctx.addStep("empty", nullptr);

    const auto r = ctx.build();
    CHECK(r.isFailed());
    CHECK(r.error() == StartupError::OwnerStartFailed);
    CHECK(!ctx.isBuilt());
}

DROGON_TEST(AppContextSecondBuildIsRejected)
{
    AppContext ctx;
    int runs = 0;
    ctx.addStep("once", [&] { ++runs; return StartupResult::ok(); });

    CHECK(ctx.build().isOk());
    const auto again = ctx.build();
    CHECK(again.isFailed());
    CHECK(again.error() == StartupError::AlreadyBuilt);
    CHECK(runs == 1);
}

/// G6：SIGTERM 与主动停机可能并发到达，重复 shutdown 不得二次 join 已析构的 owner。
DROGON_TEST(AppContextShutdownIsIdempotentAndReverseOrdered)
{
    AppContext ctx;
    std::vector<std::string> trace;

    ctx.addStep("start-all", [&] {
        ctx.addOwner("reaper", [&](std::chrono::steady_clock::time_point) { trace.push_back("stop-reaper"); });
        ctx.addOwner("account-workers", [&](std::chrono::steady_clock::time_point) { trace.push_back("stop-account-workers"); });
        ctx.addOwner("task-queue", [&](std::chrono::steady_clock::time_point) { trace.push_back("stop-task-queue"); });
        return StartupResult::ok();
    });

    CHECK(ctx.build().isOk());
    CHECK(ctx.ownersStarted() == 3);

    ctx.shutdown(soon());
    CHECK(ctx.isShutdown());

    const std::vector<std::string> expected{
        "stop-task-queue", "stop-account-workers", "stop-reaper"};
    CHECK(trace == expected);

    ctx.shutdown(soon());
    CHECK(trace == expected);  // 二次调用无新增停机动作
}

/**
 * deadline 已过时仍必须停 owner。跳过会把线程留给进程退出时强杀，正是 N4
 * 修掉的那类问题；本用例锁住「超时只记录、不放弃 join」这一决定。
 */
DROGON_TEST(AppContextShutdownStopsOwnersEvenPastDeadline)
{
    AppContext ctx;
    std::vector<std::string> trace;

    ctx.addStep("start", [&] {
        ctx.addOwner("slow", [&](std::chrono::steady_clock::time_point) { trace.push_back("stop-slow"); });
        return StartupResult::ok();
    });
    CHECK(ctx.build().isOk());

    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    ctx.shutdown(past);

    REQUIRE(trace.size() == 1);
    CHECK(trace[0] == "stop-slow");
}

/// 无步骤、无 owner 的空上下文应干净通过，避免装配尚未迁完时误报失败。
DROGON_TEST(AppContextEmptyBuildAndShutdownAreClean)
{
    AppContext ctx;
    const auto r = ctx.build();
    CHECK(r.isOk());
    CHECK(ctx.stepsCompleted() == 0);
    CHECK(ctx.degradedReasons().empty());
    ctx.shutdown(soon());
    CHECK(ctx.isShutdown());
}

/**
 * 锁住 shutdown 后的**终态**语义，而不只是「不重复停同一个 owner」。
 *
 * 变异探针发现：仅靠 stopOwnersInReverse() 末尾的 owners_.clear()，去掉
 * shutdown() 的重复调用短路后所有用例仍全绿——幂等被 clear() 偶然satisfied，
 * 短路分支实际未被任何断言覆盖。而两者语义并不等价：停机开始后若还有代码
 * 登记 owner（例如 SIGTERM 与主动停机并发、或某 owner 的 stop 内部又注册了
 * 清理动作），clear() 版本会去停这个迟到的 owner，短路版本则拒绝。后者才是
 * 想要的——停机已经宣告完成，此后不应再有新的 teardown 被静默执行。
 */
DROGON_TEST(AppContextShutdownIsTerminalAndIgnoresLateOwners)
{
    AppContext ctx;
    std::vector<std::string> trace;

    ctx.addStep("start", [&] {
        ctx.addOwner("early", [&](std::chrono::steady_clock::time_point) { trace.push_back("stop-early"); });
        return StartupResult::ok();
    });
    CHECK(ctx.build().isOk());

    ctx.shutdown(soon());
    REQUIRE(trace.size() == 1);
    CHECK(trace[0] == "stop-early");
    CHECK(ctx.isShutdown());

    // 停机宣告完成之后迟到的 owner：不得被后续 shutdown 执行。
    ctx.addOwner("late", [&](std::chrono::steady_clock::time_point) { trace.push_back("stop-late"); });
    ctx.shutdown(soon());

    REQUIRE(trace.size() == 1);
    CHECK(trace[0] == "stop-early");
}

/*
 * D3 的核心不变量：deadline 是被**传递**的，不是被各 owner 重新起算的。
 *
 * 没有这条断言，stopOwnersInReverse 里把 it->stop(deadline) 误写成
 * it->stop(std::chrono::steady_clock::now() + kSomething) 也能让全部既有用例
 * 通过——因为其余用例都丢弃了这个形参。这正是 H1 复发的最短路径。
 */
DROGON_TEST(AppContextPassesExactDeadlineToOwners)
{
    lifecycle::AppContext ctx;

    std::chrono::steady_clock::time_point seen{};
    bool called = false;
    ctx.addOwner("probe", [&](std::chrono::steady_clock::time_point deadline) {
        seen   = deadline;
        called = true;
    });

    const auto expected = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    ctx.shutdown(expected);

    CHECK(called);
    // 严格相等而非「相近」：相近会容忍「owner 自己 now()+7s」这一错误实现。
    CHECK(seen == expected);
}

/*
 * 五个 owner 必须共享同一份预算，而不是每人一份。
 *
 * H3 的成因就是各段各拿一份相对超时、总时长变成各段之和。若哪天有人在
 * stopOwnersInReverse 里给每个 owner 续期，这条用例会立刻变红。
 */
DROGON_TEST(AppContextAllOwnersShareOneDeadline)
{
    lifecycle::AppContext ctx;

    std::vector<std::chrono::steady_clock::time_point> seen;
    for (const char* name : {"first", "second", "third"}) {
        ctx.addOwner(name, [&seen](std::chrono::steady_clock::time_point deadline) {
            seen.push_back(deadline);
        });
    }

    const auto expected = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    ctx.shutdown(expected);

    REQUIRE(seen.size() == 3);
    for (const auto& d : seen) {
        CHECK(d == expected);
    }
}

/*
 * 回滚路径（build() 失败）同样要把 deadline 送到 owner。
 *
 * 回滚用的是 5s 兜底 deadline，与 shutdown() 的来源不同，是一条独立分支——
 * 独立分支就有独立漏改的可能，故单独断言 owner 确实收到了一个未来时刻。
 */
DROGON_TEST(AppContextRollbackAlsoDeliversDeadline)
{
    lifecycle::AppContext ctx;

    std::chrono::steady_clock::time_point seen{};
    bool called = false;

    ctx.addStep("start owner", [&ctx, &seen, &called] {
        ctx.addOwner("probe", [&](std::chrono::steady_clock::time_point deadline) {
            seen   = deadline;
            called = true;
        });
        return lifecycle::StartupResult::ok();
    });
    ctx.addStep("boom", [] {
        return lifecycle::StartupResult::failed(lifecycle::StartupError::OwnerStartFailed,
                                                "intentional");
    });

    const auto before = std::chrono::steady_clock::now();
    const auto result = ctx.build();

    CHECK(result.isFailed());
    CHECK(called);
    // 兜底 deadline 必须落在未来：传 now() 或默认构造值都会让 owner 一进来就判定超支。
    CHECK(seen > before);
}
