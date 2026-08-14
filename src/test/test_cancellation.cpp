#include <drogon/drogon_test.h>

#include <platform/Cancellation.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using platform::CancellationSource;

DROGON_TEST(Cancellation_FreshSourceIsNotCancelled)
{
    CancellationSource source;
    CHECK(!source.isCancelled());
}

DROGON_TEST(Cancellation_RequestIsIdempotent)
{
    // 幂等是停机路径的硬要求：AppContext::shutdown 已是幂等的，若取消源不幂等，
    // 二次 shutdown 会把「已取消」重新算作一次新的状态变更。
    CancellationSource source;
    source.request();
    CHECK(source.isCancelled());
    source.request();
    CHECK(source.isCancelled());
}

DROGON_TEST(Cancellation_WaitUntilReturnsFalseOnDeadline)
{
    // 未取消时必须等到 deadline 并报 false —— 这是周期轮询的正常路径。
    CancellationSource source;
    const auto start = std::chrono::steady_clock::now();
    const bool cancelled = source.waitUntil(start + 50ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(!cancelled);
    CHECK(elapsed >= 45ms);
}

DROGON_TEST(Cancellation_WaitUntilWakesImmediatelyWhenAlreadyCancelled)
{
    // 先取消后等待：不得再睡满一个周期。这正是 Reaper 停机退化的反面。
    CancellationSource source;
    source.request();
    const auto start = std::chrono::steady_clock::now();
    const bool cancelled = source.waitUntil(start + 5s);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(cancelled);
    CHECK(elapsed < 1s);
}

DROGON_TEST(Cancellation_RequestFromAnotherThreadInterruptsLongWait)
{
    // 核心不变量：跨线程取消必须打断长等待，且不丢失唤醒。
    // 等待方给 5s 上限、取消方 30ms 后置位，若丢失唤醒则本用例耗时会跳到 5s。
    auto source = std::make_shared<CancellationSource>();
    const auto start = std::chrono::steady_clock::now();

    std::thread canceller([source]() {
        std::this_thread::sleep_for(30ms);
        source->request();
    });

    const bool cancelled = source->waitUntil(start + 5s);
    canceller.join();

    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(cancelled);
    CHECK(elapsed < 2s);
}

DROGON_TEST(Cancellation_PastDeadlineDoesNotBlock)
{
    // deadline 已过：立即返回 false，不得阻塞。stopOwnersInReverse 在超支后
    // 仍会逐个调用 stop()，若此处阻塞，超支会被二次放大。
    CancellationSource source;
    const auto start = std::chrono::steady_clock::now();
    const bool cancelled = source.waitUntil(start - 1s);
    CHECK(!cancelled);
    CHECK(std::chrono::steady_clock::now() - start < 500ms);
}

DROGON_TEST(Cancellation_WaitForDelegatesToAbsoluteDeadline)
{
    CancellationSource source;
    source.request();
    const auto start = std::chrono::steady_clock::now();
    CHECK(source.waitFor(5s));
    CHECK(std::chrono::steady_clock::now() - start < 1s);
}

DROGON_TEST(Cancellation_ReadOnlyTokenObservesSourceAndOutlivesIt)
{
    platform::CancellationToken token;
    {
        CancellationSource source;
        token = source.token();
        CHECK(!token.isCancelled());
        source.request();
        CHECK(token.isCancelled());
    }

    // ProviderCallContext may outlive a caller-owned source during teardown;
    // the read-only token keeps the shared state valid without exposing a
    // request() operation to the provider.
    CHECK(token.isCancelled());
}
