#include <drogon/drogon_test.h>
#include <atomic>
#include <chrono>
#include <thread>

#include "utils/BackgroundTaskQueue.h"

// N2 停机 fail-fast 验证
//
// 注意：BackgroundTaskQueue 是进程级单例，shutdownCalled_ 是不可逆标志。
// 因此本文件内的用例对同一实例有顺序依赖：一旦调用 shutdown()，
// 后续所有 enqueue 都必须失败。drogon_test 在同一进程内顺序执行，
// 故把「停机前」与「停机后」两阶段断言合并在单个 TEST 中，
// 避免依赖用例间的执行顺序。
DROGON_TEST(BackgroundTaskQueue_ShutdownIsIrreversible)
{
    auto& q = BackgroundTaskQueue::instance();

    // 阶段一：停机前，enqueue 必须被接受并真正执行
    std::atomic<int> executed{0};
    for (int i = 0; i < 4; ++i) {
        const bool accepted =
            q.enqueue("pre-shutdown", [&executed]() { executed.fetch_add(1); });
        CHECK(accepted);
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
    const bool rejected =
        q.enqueue("post-shutdown", [&zombieRan]() { zombieRan.store(true); });
    CHECK(rejected == false);

    // 被拒的任务绝不能被执行。给一个宽限窗口，
    // 若线程池真的复活了，这里足以捕获到。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(zombieRan.load() == false);
    CHECK(q.pendingCount() == 0);

    // 阶段四：shutdown() 幂等，重复调用不得挂起或崩溃
    q.shutdown();
    q.shutdown();
    CHECK(q.enqueue("after-double-shutdown", []() {}) == false);
}
