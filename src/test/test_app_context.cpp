#include <drogon/drogon_test.h>

#include <runtime/AppContext.h>
#include <runtime/StartupResult.h>

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

}  // namespace

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
        ctx.addOwner("A", [&] { trace.push_back("stop-A"); });
        return StartupResult::ok();
    });
    ctx.addStep("start-B", [&] {
        trace.push_back("start-B");
        ctx.addOwner("B", [&] { trace.push_back("stop-B"); });
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
        ctx.addOwner("reaper", [&] { trace.push_back("stop-reaper"); });
        ctx.addOwner("account-workers", [&] { trace.push_back("stop-account-workers"); });
        ctx.addOwner("task-queue", [&] { trace.push_back("stop-task-queue"); });
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
        ctx.addOwner("slow", [&] { trace.push_back("stop-slow"); });
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
        ctx.addOwner("early", [&] { trace.push_back("stop-early"); });
        return StartupResult::ok();
    });
    CHECK(ctx.build().isOk());

    ctx.shutdown(soon());
    REQUIRE(trace.size() == 1);
    CHECK(trace[0] == "stop-early");
    CHECK(ctx.isShutdown());

    // 停机宣告完成之后迟到的 owner：不得被后续 shutdown 执行。
    ctx.addOwner("late", [&] { trace.push_back("stop-late"); });
    ctx.shutdown(soon());

    REQUIRE(trace.size() == 1);
    CHECK(trace[0] == "stop-early");
}
