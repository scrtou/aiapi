#include <drogon/drogon_test.h>

#include <domain/port/IErrorStatsSink.h>
#include <metrics/ErrorStatsConfig.h>
#include <metrics/ErrorStatsService.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

// P4-W3 / D4：shutdown(deadline) 超预算路径的回归。
//
// 存在理由（务必保留）：本文件的姊妹用例 test_error_stats_shutdown_deadline.cpp
// 全部走「worker 秒退」的正常路径，实测证明它们【杀不掉】
// 「把 joinUntil 换成无限 join」这个变异 —— 正常路径下两者一样快，绿灯是免费的。
// 只有把 worker 真正拖住，限时汇合与无限等待才可区分。
//
// 手法：worker 退出前会执行 flushEvents()，其落库走可注入的 IErrorStatsSink。
// 注入一个在 insertEvents 里长睡的 sink，即可让 worker 卡在预算之外。

namespace
{
using namespace std::chrono_literals;

constexpr auto kSinkStall = 3s;

class StallingSink : public metrics::IErrorStatsSink
{
  public:
    std::atomic<bool> entered{false};

    void init() override {}

    bool insertEvents(const std::vector<metrics::ErrorEvent>&) override
    {
        entered.store(true);
        std::this_thread::sleep_for(kSinkStall);
        return true;
    }

    bool upsertErrorAggHour(const std::vector<metrics::ErrorEvent>&) override { return true; }
    bool upsertRequestAggHour(const metrics::RequestAggData&) override { return true; }
    int cleanupOldEvents(int) override { return 0; }
    int cleanupOldAgg(int) override { return 0; }
};

metrics::ErrorStatsConfig makeConfig()
{
    metrics::ErrorStatsConfig c;
    c.enabled = true;
    c.asyncBatchSize = 1;
    c.asyncFlushMs = 10;
    return c;
}

}  // namespace

DROGON_TEST(ErrorStatsShutdown_BudgetExceededReturnsFalseAndStaysBounded)
{
    auto sink = std::make_shared<StallingSink>();
    auto& service = metrics::ErrorStatsService::getInstance();
    service.setSink(sink);
    service.init(makeConfig());
    // 前置：确认这一轮 init() 真的拉起了 worker，而不是撞上未复位的 initialized_
    // 变成 no-op —— 否则下面的「超预算」会由「根本没线程」冒充。
    REQUIRE(service.isRunning());

    // 入队一条事件并等 worker 真正进入 sink，确保它已被拖住。
    service.recordError(metrics::Domain::INTERNAL,
                        "internal.shutdown_budget_probe",
                        "stall the worker inside the sink",
                        "req-shutdown-budget-1");

    for (int i = 0; i < 200 && !sink->entered.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(sink->entered.load());

    const auto start = std::chrono::steady_clock::now();
    const bool joined = service.shutdown(start + 200ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // (a) 语义：预算内没等到线程，必须如实返回 false。
    CHECK(joined == false);
    // (b) 时间上界：这一条才是杀手锏。若 shutdown 忽略 deadline 改用无限 join，
    //     它会一直等到 sink 睡满 3s，此处必红。上界取 1s，远小于 kSinkStall。
    CHECK(elapsed < 1s);

    // 超预算时线程未被 detach，仍归本对象。用无参重载把它收割干净，
    // 否则它会带着即将析构的 sink 继续运行，且下个用例的 init() 会对一个
    // joinable 的 std::thread 赋值，直接 std::terminate。
    // 收割前：running_ 早在上面的限时 shutdown 里就置了 false，单看它什么都证明不了；
    // hasPendingWorker() 才是「线程仍在、未被 detach」的实证。
    CHECK(service.hasPendingWorker());
    service.shutdown();
    CHECK(service.hasPendingWorker() == false);
    CHECK(service.isRunning() == false);
}
