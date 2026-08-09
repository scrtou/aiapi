#include <drogon/drogon_test.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <stdexcept>

#include <utils/BackgroundTaskQueue.h>

// N2 停机 fail-fast 验证
//
// 状态机单向不可逆，shutdown() 后实例永久处于 Stopped。若共用进程级
// 单例，先跑的用例会把后续用例全部拖入 Stopped —— 断言失败并非实现
// 缺陷，而是夹具串扰。故每个用例用 createForTesting() 持有独立实例。
DROGON_TEST(BackgroundTaskQueue_ShutdownIsIrreversible)
{
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;

    // 阶段一：停机前，enqueue 必须被接受并真正执行
    std::atomic<int> executed{0};
    for (int i = 0; i < 4; ++i) {
        const auto accepted =
            q.enqueue("pre-shutdown", [&executed]() { executed.fetch_add(1); });
        CHECK(accepted == EnqueueResult::Accepted);
    }

    // 阶段二：shutdown() 语义是 drain + join，返回后队列必须已排空，
    // 且 4 个任务全部执行完毕 —— 这同时验证了「不丢任务」。
    q.shutdown();
    CHECK(q.pendingCount() == 0);
    CHECK(executed.load() == 4);

    // 阶段三：停机后 enqueue 必须 fail-fast。
    // 这是 N2 的核心：旧实现中 shutdown() 会置 started_=false，
    // 迟到的 enqueue 命中 `if (!started_)` 分支重新 spawn 8 条线程并把
    // stopping_ 复位，导致进程永不退出。
    std::atomic<bool> zombieRan{false};
    const auto rejected =
        q.enqueue("post-shutdown", [&zombieRan]() { zombieRan.store(true); });
    CHECK(rejected == EnqueueResult::Stopped);

    // 被拒的任务绝不能被执行。给一个宽限窗口，
    // 若线程池真的复活了，这里足以捕获到。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(zombieRan.load() == false);
    CHECK(q.pendingCount() == 0);

    // 阶段四：shutdown() 幂等，重复调用不得挂起或崩溃
    q.shutdown();
    q.shutdown();
    CHECK(q.enqueue("after-double-shutdown", []() {}) == EnqueueResult::Stopped);
}

DROGON_TEST(BackgroundTaskQueue_ShutdownWaitsForRunningTask)
{
    using namespace std::chrono_literals;
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;
    q.start(1);

    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    REQUIRE(q.enqueue("blocking-task", [&entered, releaseFuture]() mutable {
        entered.set_value();
        releaseFuture.wait();
    }) == EnqueueResult::Accepted);
    REQUIRE(enteredFuture.wait_for(2s) == std::future_status::ready);

    auto stopped = std::async(std::launch::async, [&q] { q.shutdown(); });
    CHECK(stopped.wait_for(100ms) == std::future_status::timeout);
    release.set_value();
    REQUIRE(stopped.wait_for(2s) == std::future_status::ready);
    stopped.get();
    CHECK(q.pendingCount() == 0);
}

DROGON_TEST(BackgroundTaskQueue_ShutdownDrainsBacklogAndRejectsLateWork)
{
    using namespace std::chrono_literals;
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;
    q.start(2);

    std::atomic<int> entered{0};
    std::atomic<int> executed{0};
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    for (int i = 0; i < 2; ++i) {
        REQUIRE(q.enqueue("occupied-worker", [&entered, &executed, releaseFuture]() mutable {
            entered.fetch_add(1);
            releaseFuture.wait();
            executed.fetch_add(1);
        }) == EnqueueResult::Accepted);
    }
    for (int i = 0; i < 100 && entered.load() != 2; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(entered.load() == 2);

    int accepted = 2;
    constexpr int kBacklog = 24;
    for (int i = 0; i < kBacklog; ++i) {
        REQUIRE(q.enqueue("backlog", [&executed] { executed.fetch_add(1); })
                == EnqueueResult::Accepted);
        ++accepted;
    }

    auto stopped = std::async(std::launch::async, [&q] { q.shutdown(); });
    // Wait until shutdown has closed the mutex-protected enqueue boundary.
    // Unlike a timing sleep, this remains deterministic under coverage/ASan.
    for (int i = 0; i < 200 && q.acceptingTasks(); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(!q.acceptingTasks());
    CHECK(q.enqueue("late-work", [&executed] { executed.fetch_add(1); })
          == EnqueueResult::ShuttingDown);
    CHECK(stopped.wait_for(100ms) == std::future_status::timeout);
    release.set_value();
    REQUIRE(stopped.wait_for(3s) == std::future_status::ready);
    stopped.get();
    CHECK(executed.load() == accepted);
    CHECK(q.pendingCount() == 0);
    CHECK(q.enqueue("after-drain", [] {}) == EnqueueResult::Stopped);
}

// ---------------------------------------------------------------------------
// P4-W1 / C2：容量上限与背压；Draining 拒绝递归入队
//
// 本文件用 Drogon 自带的 DROGON_TEST/CHECK/REQUIRE，不是 Catch2；
// 上一版误用 TEST_CASE 导致编译失败，这里统一成仓库现有写法。
// 同样每个用例持独立实例，避免单例的不可逆状态机串扰。
// ---------------------------------------------------------------------------

DROGON_TEST(BackgroundTaskQueue_QueueFullAppliesBackpressure)
{
    using namespace std::chrono_literals;
    auto qOwned = BackgroundTaskQueue::createForTesting(/*capacity=*/2);
    auto& q = *qOwned;
    q.start(1);

    // 占住唯一的工作线程，之后入队的任务只会堆在队列里
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();

    REQUIRE(q.enqueue("blocker", [&entered, releaseFuture]() mutable {
                entered.set_value();
                releaseFuture.wait();
            }) == EnqueueResult::Accepted);
    REQUIRE(enteredFuture.wait_for(2s) == std::future_status::ready);

    REQUIRE(q.enqueue("fill-1", [] {}) == EnqueueResult::Accepted);
    REQUIRE(q.enqueue("fill-2", [] {}) == EnqueueResult::Accepted);
    REQUIRE(q.pendingCount() == 2);

    // 第三个越过 capacity=2，必须是显式背压而不是无声接受。
    // 旧实现无界队列，过载时只会内存持续增长，调用方拿不到任何信号。
    CHECK(q.enqueue("overflow", [] {}) == EnqueueResult::QueueFull);
    CHECK(q.pendingCount() == 2);

    release.set_value();
    q.shutdown();
    CHECK(q.pendingCount() == 0);
}

DROGON_TEST(BackgroundTaskQueue_QueueFullDoesNotAutoStartWorkers)
{
    // 容量判断必须【先于】自动启动分支：一个注定被拒的任务不应该反过来
    // spawn 8 条工作线程。
    //
    // 上一版用 capacity=1 + 先塞一个阻塞任务来构造「队列已满」，但那个任务
    // 会被自动启动的 worker 立刻取走，pendingCount 归零，第二次入队自然是
    // Accepted —— 测的是调度时序而不是分支顺序。这里改用 capacity=0：
    // Fresh 状态下第一次 enqueue 就必然撞上容量上限，不依赖任何时序。
    auto qOwned = BackgroundTaskQueue::createForTesting(/*capacity=*/0);
    auto& q = *qOwned;

    REQUIRE(q.workerCount() == 0);
    CHECK(q.enqueue("rejected-before-spawn", [] {}) == EnqueueResult::QueueFull);

    // 核心断言：被拒之后线程池仍未启动，队列也没留下残留任务。
    CHECK(q.workerCount() == 0);
    CHECK(q.pendingCount() == 0);

    // 状态未被污染：仍是 Fresh，显式 start() 之后可正常工作。
    CHECK(q.acceptingTasks());

    q.shutdown();
}

DROGON_TEST(BackgroundTaskQueue_DrainingRejectsRecursiveEnqueue)
{
    using namespace std::chrono_literals;
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;
    q.start(1);

    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    std::atomic<int> innerResult{-1};

    REQUIRE(q.enqueue("outer", [&entered, &innerResult, &q, releaseFuture]() mutable {
                entered.set_value();
                releaseFuture.wait();
                // 此刻队列已进入 Draining：任务内再入队必须被拒，
                // 否则 drain 可以被自身无限延长，停机永不收敛。
                innerResult.store(
                    static_cast<int>(q.enqueue("inner", [] {})));
            }) == EnqueueResult::Accepted);
    REQUIRE(enteredFuture.wait_for(2s) == std::future_status::ready);

    auto stopped = std::async(std::launch::async, [&q]() { q.shutdown(); });

    // 等 shutdown() 真正把状态推到 Draining，再放行任务体
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (q.acceptingTasks() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(!q.acceptingTasks());

    release.set_value();
    REQUIRE(stopped.wait_for(3s) == std::future_status::ready);

    CHECK(innerResult.load() ==
          static_cast<int>(EnqueueResult::ShuttingDown));
    CHECK(q.pendingCount() == 0);
}

// ---------------------------------------------------------------------------
// C3: waitUntilIdle 完成通知契约
// ---------------------------------------------------------------------------

// 回归意图：空闲判定若只看 tasks_.empty()，会在「最后一个任务已出队、仍在
// 执行」的窗口内误报空闲。用单线程队列稳定制造该窗口。
DROGON_TEST(BackgroundTaskQueue_WaitUntilIdleObservesRunningTask)
{
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;
    q.start(1);

    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};

    CHECK(q.enqueue("long-task", [&entered, &finished, releaseFuture]() {
              entered.store(true);
              releaseFuture.wait();
              finished.store(true);
          }) == EnqueueResult::Accepted);

    while (!entered.load()) std::this_thread::yield();

    CHECK(q.pendingCount() == 0);
    CHECK(q.runningCount() == 1);
    CHECK(q.waitUntilIdle(std::chrono::milliseconds(50)) == false);
    CHECK(finished.load() == false);

    release.set_value();
    CHECK(q.waitUntilIdle(std::chrono::seconds(5)) == true);
    CHECK(finished.load() == true);
    CHECK(q.runningCount() == 0);

    q.shutdown();
}

// 回归意图：递减若放在 try 内部，抛异常的任务会让 running_ 永不归零，
// waitUntilIdle 将永久挂起。
DROGON_TEST(BackgroundTaskQueue_WaitUntilIdleRecoversFromThrowingTask)
{
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;
    q.start(2);

    CHECK(q.enqueue("throwing", []() {
              throw std::runtime_error("boom");
          }) == EnqueueResult::Accepted);

    CHECK(q.waitUntilIdle(std::chrono::seconds(5)) == true);
    CHECK(q.runningCount() == 0);

    q.shutdown();
}

// 回归意图：waitUntilIdle 只观测不推进状态机。调用后队列必须仍能接受任务，
// 否则 AppContext 用它做健康探测会意外把队列关掉。
DROGON_TEST(BackgroundTaskQueue_WaitUntilIdleDoesNotAdvanceStateMachine)
{
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;
    q.start(2);

    CHECK(q.waitUntilIdle(std::chrono::seconds(5)) == true);
    CHECK(q.acceptingTasks() == true);

    std::atomic<int> ran{0};
    CHECK(q.enqueue("after-wait", [&ran]() { ran.fetch_add(1); })
          == EnqueueResult::Accepted);
    CHECK(q.waitUntilIdle(std::chrono::seconds(5)) == true);
    CHECK(ran.load() == 1);

    q.shutdown();
}

// Fresh 状态没有 worker，也没有工作——应立即判定空闲，不得空等满 timeout。
DROGON_TEST(BackgroundTaskQueue_WaitUntilIdleOnFreshQueueReturnsImmediately)
{
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;

    const auto t0 = std::chrono::steady_clock::now();
    CHECK(q.waitUntilIdle(std::chrono::seconds(5)) == true);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed < std::chrono::seconds(1));
}

// drain 语义：waitUntilIdle 返回 true 后的完成集合必须与 shutdown join 后一致。
DROGON_TEST(BackgroundTaskQueue_WaitUntilIdleAgreesWithDrainOnShutdown)
{
    auto qOwned = BackgroundTaskQueue::createForTesting();
    auto& q = *qOwned;
    q.start(4);

    std::atomic<int> done{0};
    constexpr int kTasks = 64;
    for (int i = 0; i < kTasks; ++i) {
        CHECK(q.enqueue("drain-task", [&done]() {
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
                  done.fetch_add(1);
              }) == EnqueueResult::Accepted);
    }

    CHECK(q.waitUntilIdle(std::chrono::seconds(30)) == true);
    CHECK(done.load() == kTasks);
    CHECK(q.pendingCount() == 0);
    CHECK(q.runningCount() == 0);

    q.shutdown();
    CHECK(done.load() == kTasks);
}
