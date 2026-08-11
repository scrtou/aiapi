#include <drogon/drogon_test.h>

#include <platform/ThreadJoin.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using platform::ThreadCompletion;
using platform::joinUntil;

DROGON_TEST(ThreadJoin_JoinsWithinBudgetWhenWorkerCompletes)
{
    // 正常停机路径：worker 及时自报完成，joinUntil 必须在预算内汇合并返回 true。
    ThreadCompletion completion;
    std::atomic<bool> ran{false};
    std::thread worker([&completion, &ran]() {
        ran = true;
        completion.signal();
    });

    const auto start = std::chrono::steady_clock::now();
    const bool joined = joinUntil(worker, completion, start + 5s);

    CHECK(joined);
    CHECK(ran.load());
    CHECK(!worker.joinable());
    CHECK(std::chrono::steady_clock::now() - start < 2s);
}

DROGON_TEST(ThreadJoin_ReportsFalseWhenWorkerMissesDeadline)
{
    // H5 的核心：worker 卡住时 joinUntil 必须在预算耗尽后【返回】，而不是像
    // 裸 join() 那样一直挂住。返回 false 就是「超预算」这一可观测事实。
    ThreadCompletion completion;
    std::atomic<bool> release{false};
    std::thread worker([&completion, &release]() {
        while (!release.load()) {
            std::this_thread::sleep_for(5ms);
        }
        completion.signal();
    });

    const auto start = std::chrono::steady_clock::now();
    const bool joined = joinUntil(worker, completion, start + 100ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(!joined);
    CHECK(elapsed >= 90ms);
    // 关键：超时路径【不】detach、【不】join，线程对象仍归调用方处置。
    CHECK(worker.joinable());

    release = true;
    worker.join();
}

DROGON_TEST(ThreadJoin_TimeoutLeavesThreadJoinableForCaller)
{
    // 超时后调用方仍必须能正常收尾。若原语偷偷 detach，这里的 join 会崩，
    // 且退出门禁「不得析构仍被活动线程访问的对象」将无从遵守。
    ThreadCompletion completion;
    std::atomic<bool> release{false};
    std::thread worker([&completion, &release]() {
        while (!release.load()) {
            std::this_thread::sleep_for(5ms);
        }
        completion.signal();
    });

    CHECK(!joinUntil(worker, completion, std::chrono::steady_clock::now() + 50ms));
    // 所有权断言：超时【不得】改变线程归属。缺了这一条，本用例会被 detach
    // 实现「免费满足」——超时后 detach、二次调用走 non-joinable 快速路径同样全绿。
    CHECK(worker.joinable());

    release = true;
    const bool secondAttempt =
        joinUntil(worker, completion, std::chrono::steady_clock::now() + 5s);
    CHECK(secondAttempt);
    CHECK(!worker.joinable());
}

DROGON_TEST(ThreadJoin_NonJoinableThreadCountsAsJoined)
{
    // 幂等停机：AppContext::shutdown 可被调用两次，第二次线程已汇合。
    // 此时必须直接返回 true，而不是对着空线程等满一个预算。
    ThreadCompletion completion;
    std::thread empty;
    const auto start = std::chrono::steady_clock::now();
    CHECK(joinUntil(empty, completion, start + 5s));
    CHECK(std::chrono::steady_clock::now() - start < 500ms);
}

DROGON_TEST(ThreadJoin_PastDeadlineDoesNotBlock)
{
    // 预算已被前面的 owner 吃光：本 owner 必须立即返回 false，不得二次放大超支。
    ThreadCompletion completion;
    std::atomic<bool> release{false};
    std::thread worker([&release]() {
        while (!release.load()) {
            std::this_thread::sleep_for(5ms);
        }
    });

    const auto start = std::chrono::steady_clock::now();
    CHECK(!joinUntil(worker, completion, start - 1s));
    CHECK(std::chrono::steady_clock::now() - start < 500ms);

    release = true;
    worker.join();
}

DROGON_TEST(ThreadJoin_SignalIsIdempotent)
{
    ThreadCompletion completion;
    CHECK(!completion.isComplete());
    completion.signal();
    CHECK(completion.isComplete());
    completion.signal();
    CHECK(completion.isComplete());
}

DROGON_TEST(ThreadJoin_SignalFromWorkerInterruptsLongWait)
{
    // 不丢唤醒：等待方给 5s 上限、worker 30ms 后 signal。
    // 若唤醒丢失，本用例耗时会从 <2s 跳到 5s —— 与 D2 同一根断言逻辑。
    ThreadCompletion completion;
    const auto start = std::chrono::steady_clock::now();
    std::thread worker([&completion]() {
        std::this_thread::sleep_for(30ms);
        completion.signal();
    });

    const bool joined = joinUntil(worker, completion, start + 5s);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(joined);
    CHECK(elapsed < 2s);
}
