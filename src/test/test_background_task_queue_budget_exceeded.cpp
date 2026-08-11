#include <drogon/drogon_test.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <utils/BackgroundTaskQueue.h>

// ---------------------------------------------------------------------------
// P4-D5：BackgroundTaskQueue 限时停机的【超时路径】验收。
//
// 本文件用 Drogon 自带的 DROGON_TEST/CHECK/REQUIRE，不是 Catch2/doctest。
// 每个用例持独立实例（createForTesting），避免单例不可逆状态机串扰。
//
// 变异检验矩阵（勿删——这段注释是判据本身，不是说明文字）
// 下列四条已在 2026-08-11 逐条施加于源文件并实测，检出证据一并记录；
// 单跑用例的开关是 `-r <用例名>`（`-l` 列出全部），不是 -t。
//   M1 把 shutdownImpl 里的 joinUntil(...) 换回无条件 w.join()
//      实测：本文件 :57 :58 :83 三处断言变红（等满 3s，撞破 1s 时间上界）
//   M2 超预算时仍执行 workers_.clear()
//      实测：joinable 线程被析构 → terminate；-r 单跑
//      ReturnsWithinBudget 与 StaysDrainingAndReapable 均 rc=134，
//      而 JoinsAllOnHappyPath rc=0 —— 证明检出只来自超预算路径，归属干净。
//      注意：abort 不留断言行号，全量跑时无法归因，必须 -r 单跑才能定罪。
//   M3 超预算时把 state_ 推到 Stopped
//      实测：本文件 :88 :97 断言变红（二次收割不再可行）
//   M4 所有 worker 共享同一个 ThreadCompletion
//      实测：本文件 :57 :58 :83 断言变红（一条秒退的 worker 误判全体完成）
//
// 反例警示：只断言 shutdown(deadline)==false 是【不够】的——正常路径下无限
// join 同样不会触发 false 分支而全绿。时间上界那一条才是能杀死 M1 的断言。
// ---------------------------------------------------------------------------

namespace {
using Clock = std::chrono::steady_clock;
}

DROGON_TEST(BackgroundTaskQueue_ShutdownWithDeadlineReturnsWithinBudget)
{
    using namespace std::chrono_literals;
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;
    q.start(2);

    std::atomic<bool> released{false};
    // 这条任务把一条 worker 拖到远超预算之外。
    REQUIRE(q.enqueue("stuck", [&released]() {
        std::this_thread::sleep_for(3s);
        released = true;
    }) == EnqueueResult::Accepted);

    // 等它真的出队并开始执行，否则可能在任务尚未被取走时就 drain 完了。
    while (q.runningCount() == 0) {
        std::this_thread::sleep_for(5ms);
    }

    const auto t0 = Clock::now();
    const bool joined = q.shutdown(t0 + 200ms);
    const auto elapsed = Clock::now() - t0;

    // 双侧断言：返回值语义 + 时间上界。缺任一侧都杀不掉 M1。
    CHECK(joined == false);
    CHECK(elapsed < 1s);

    // 收尾：等任务自然结束后用无参重载收割，避免残留线程影响后续用例。
    while (!released) {
        std::this_thread::sleep_for(10ms);
    }
    q.shutdown();
}

DROGON_TEST(BackgroundTaskQueue_BudgetExceededStaysDrainingAndReapable)
{
    using namespace std::chrono_literals;
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;
    q.start(2);

    std::atomic<bool> released{false};
    REQUIRE(q.enqueue("stuck", [&released]() {
        std::this_thread::sleep_for(2s);
        released = true;
    }) == EnqueueResult::Accepted);
    while (q.runningCount() == 0) {
        std::this_thread::sleep_for(5ms);
    }

    REQUIRE(q.shutdown(Clock::now() + 150ms) == false);

    // 超预算 != 停机失败：状态机必须已离开 Running（不再收新任务），
    // 但不得跳到 Stopped——否则残留线程无从辨认，也无从收割。
    CHECK(q.acceptingTasks() == false);
    CHECK(q.enqueue("late", []() {}) == EnqueueResult::ShuttingDown);
    // 线程仍被本对象持有：既没 join 也没 detach，更没被 clear 掉。
    CHECK(q.workerCount() == 2);

    while (!released) {
        std::this_thread::sleep_for(10ms);
    }
    // 二次收割：无参重载在 Draining 上必须能把残局收干净。
    q.shutdown();
    CHECK(q.workerCount() == 0);
    CHECK(q.enqueue("after", []() {}) == EnqueueResult::Stopped);
}

DROGON_TEST(BackgroundTaskQueue_ShutdownWithDeadlineJoinsAllOnHappyPath)
{
    using namespace std::chrono_literals;
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;
    q.start(4);

    std::atomic<int> done{0};
    for (int i = 0; i < 8; ++i) {
        REQUIRE(q.enqueue("quick", [&done]() { ++done; }) == EnqueueResult::Accepted);
    }
    // 4 条线程共用一个绝对 deadline，总上界是 deadline 本身而非 4 倍预算。
    CHECK(q.shutdown(Clock::now() + 5s) == true);
    CHECK(done.load() == 8);
    CHECK(q.workerCount() == 0);
}
