// ===========================================================================
// 【变异检验矩阵 —— 本文件用例的实测杀伤力，勿删】
//
// 2026-08-11 实测（注入缺陷后跑全量，看是否变红）：
//
//   变异A：删除 ErrorStatsService::shutdown 中的 `initialized_ = false;`
//          => 本文件变红。故「重复 init 能重新拉起 worker」这一约束由本文件守住。
//
//   变异B：把 shutdown(deadline) 里的 platform::joinUntil(...) 换成
//          无条件 workerThread_.join()（即完全忽略时间预算）
//          => 本文件【全绿，杀不掉】。
//
// 结论：本文件的三个用例都走「worker 秒退」的正常路径，无限等待与限时汇合
// 在该路径下耗时相同，绿灯是免费拿到的。它们覆盖的是正常路径与幂等语义，
// 【不能】证明时间预算真的被接上。
//
// 真正约束 deadline 的是 test_error_stats_shutdown_budget_exceeded.cpp：
// 它注入长睡 sink 把 worker 拖到预算之外，并断言 shutdown 的返回值与时间上界，
// 变异B 在那里会变红。改动限时停机逻辑时，务必同时看那个文件。
// ===========================================================================

#include <drogon/drogon_test.h>

#include <metrics/ErrorStatsService.h>
#include <domain/port/IErrorStatsSink.h>

#include <chrono>
#include <memory>
#include <vector>

// P4-W3 / D4 —— J5：ErrorStatsService 停机接入 deadline 的契约测试。
//
// 背景（doc/adr/work-products/P04-shutdown-deadline.md §8.1）：
// AppWiring 的 error stats 闭包此前拿到 deadline 后只打一条日志，随后调用无参
// shutdown()。签名传播完成、语义传播缺失，停机在此处仍无上限。
//
// 刻意**不**在这里测「worker 卡住导致超预算」：ErrorStatsService 的 workerLoop
// 收到 notify 即退出，构造不出稳定的卡死场景，用 sleep 硬凑只会得到偶发绿。
// 超时路径的行为由 D3 原语层 test_thread_join.cpp 覆盖（超期返回 false、线程
// 所有权不变）；此处只验「J5 是否真把 deadline 接到了那条限时汇合路径上」。

using namespace std::chrono_literals;

namespace {

class NoopSink final : public metrics::IErrorStatsSink
{
  public:
    void init() override {}
    bool insertEvents(const std::vector<metrics::ErrorEvent>&) override { return true; }
    bool upsertErrorAggHour(const std::vector<metrics::ErrorEvent>&) override { return true; }
    bool upsertRequestAggHour(const metrics::RequestAggData&) override { return true; }
    int cleanupOldEvents(int) override { return 0; }
    int cleanupOldAgg(int) override { return 0; }
};

metrics::ErrorStatsConfig makeConfig()
{
    metrics::ErrorStatsConfig config;
    config.enabled        = true;
    config.asyncBatchSize = 200;
    config.asyncFlushMs   = 50;
    return config;
}

}  // namespace

DROGON_TEST(ErrorStatsShutdown_JoinsWithinBudget)
{
    metrics::ErrorStatsService service(std::make_shared<NoopSink>());
    service.init(makeConfig());
    REQUIRE(service.isRunning());

    const auto start  = std::chrono::steady_clock::now();
    // 返回值即「是否在预算内汇合」——这是 H3/H5 要求的可观测事实。
    const bool joined = service.shutdown(start + 5s);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(joined);
    // 预算 5s，正常退出应远快于此；留 2s 宽限容忍 CI 抖动。
    CHECK(elapsed < 2s);
}

DROGON_TEST(ErrorStatsShutdown_ExpiredDeadlineStaysBounded)
{
    metrics::ErrorStatsService service(std::make_shared<NoopSink>());
    service.init(makeConfig());
    REQUIRE(service.isRunning());

    // 过期 deadline：worker 仍会因 notify 迅速退出，返回值可能为 true。
    // 本用例约束的是**有界性**，不是返回值——写成 CHECK(!joined) 会依赖调度
    // 巧合，正是 D3 教训里那种「被巧合满足」的断言。
    const auto start = std::chrono::steady_clock::now();
    (void)service.shutdown(start - 1s);
    CHECK(std::chrono::steady_clock::now() - start < 2s);

    // 超预算时线程不被 detach，仍归本对象；用无参重载把它收割掉，
    // 否则局部 service 析构时会遇到仍 joinable 的线程。
    service.shutdown();
}

DROGON_TEST(ErrorStatsShutdown_NoArgOverloadKeptAndIdempotent)
{
    metrics::ErrorStatsService service(std::make_shared<NoopSink>());
    service.init(makeConfig());
    REQUIRE(service.isRunning());

    // 无参收割重载保留给析构兜底与超预算后的显式收割。
    service.shutdown();

    // 幂等：未运行时再停一次必须立即返回 true，且不得阻塞。
    const auto start  = std::chrono::steady_clock::now();
    const bool second = service.shutdown(std::chrono::steady_clock::now() + 5s);
    CHECK(second);
    CHECK(std::chrono::steady_clock::now() - start < 1s);
}
