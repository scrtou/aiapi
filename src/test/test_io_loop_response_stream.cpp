#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <trantor/net/AsyncStream.h>

#include "../utils/IoLoopResponseStream.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct StreamProbe
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> writes;
    std::thread::id writeThread;
    std::thread::id closeThread;
    int closeCount = 0;
};

class ProbeAsyncStream final : public trantor::AsyncStream
{
public:
    explicit ProbeAsyncStream(std::shared_ptr<StreamProbe> probe)
        : probe_(std::move(probe))
    {
    }

    bool send(const char *data, size_t len) override
    {
        {
            std::lock_guard<std::mutex> lock(probe_->mutex);
            probe_->writes.emplace_back(data, len);
            probe_->writeThread = std::this_thread::get_id();
        }
        probe_->cv.notify_all();
        return true;
    }

    void close() override
    {
        {
            std::lock_guard<std::mutex> lock(probe_->mutex);
            ++probe_->closeCount;
            probe_->closeThread = std::this_thread::get_id();
        }
        probe_->cv.notify_all();
    }

private:
    std::shared_ptr<StreamProbe> probe_;
};

struct DisconnectRaceProbe
{
    explicit DisconnectRaceProbe(trantor::EventLoop *ownerLoop)
        : loop(ownerLoop)
    {
    }

    trantor::EventLoop *loop;
    std::mutex mutex;
    bool connected = true;
    bool writeEventEnabled = false;
    int rejectedSends = 0;
};

class DisconnectRaceAsyncStream final : public trantor::AsyncStream
{
public:
    explicit DisconnectRaceAsyncStream(std::shared_ptr<DisconnectRaceProbe> probe)
        : probe_(std::move(probe))
    {
    }

    bool send(const char *, size_t) override
    {
        {
            std::lock_guard<std::mutex> lock(probe_->mutex);
            if (!probe_->connected)
            {
                ++probe_->rejectedSends;
                return false;
            }
        }

        auto enableWrite = [probe = probe_]() {
            std::lock_guard<std::mutex> lock(probe->mutex);
            probe->writeEventEnabled = true;
        };
        if (probe_->loop->isInLoopThread())
        {
            enableWrite();
        }
        else
        {
            probe_->loop->queueInLoop(std::move(enableWrite));
        }
        return true;
    }

    void close() override
    {
    }

private:
    std::shared_ptr<DisconnectRaceProbe> probe_;
};

std::thread::id eventLoopThreadId(trantor::EventLoop *loop)
{
    std::promise<std::thread::id> promise;
    auto future = promise.get_future();
    loop->queueInLoop([&promise]() {
        promise.set_value(std::this_thread::get_id());
    });
    return future.get();
}

bool waitForClose(const std::shared_ptr<StreamProbe> &probe)
{
    std::unique_lock<std::mutex> lock(probe->mutex);
    return probe->cv.wait_for(lock, 2s, [&probe]() {
        return probe->closeCount == 1;
    });
}

}  // namespace

DROGON_TEST(IoLoopResponseStream_SerializesWorkerOperations)
{
    auto *loop = drogon::app().getLoop();
    REQUIRE(loop != nullptr);
    const auto loopThread = eventLoopThreadId(loop);

    auto probe = std::make_shared<StreamProbe>();
    auto responseStream = std::make_unique<drogon::ResponseStream>(
        std::make_unique<ProbeAsyncStream>(probe));
    auto bridge = IoLoopResponseStream::create(std::move(responseStream), loop);
    REQUIRE(bridge != nullptr);

    std::atomic<bool> sendAccepted{false};
    std::thread worker([bridge, &sendAccepted]() {
        sendAccepted.store(bridge->send("hello"), std::memory_order_release);
        bridge->close();
    });
    worker.join();

    REQUIRE(waitForClose(probe));
    CHECK(sendAccepted.load(std::memory_order_acquire));
    std::lock_guard<std::mutex> lock(probe->mutex);
    CHECK(probe->closeCount == 1);
    CHECK(probe->writeThread == loopThread);
    CHECK(probe->closeThread == loopThread);
    REQUIRE(probe->writes.size() >= 2);
    CHECK(probe->writes.front().find("hello") != std::string::npos);
    CHECK(probe->writes.back() == "0\r\n\r\n");
}

DROGON_TEST(IoLoopResponseStream_DestructorIsLoopAffine)
{
    auto *loop = drogon::app().getLoop();
    REQUIRE(loop != nullptr);
    const auto loopThread = eventLoopThreadId(loop);

    auto probe = std::make_shared<StreamProbe>();
    auto responseStream = std::make_unique<drogon::ResponseStream>(
        std::make_unique<ProbeAsyncStream>(probe));
    auto bridge = IoLoopResponseStream::create(std::move(responseStream), loop);
    REQUIRE(bridge != nullptr);

    std::thread worker([bridge = std::move(bridge)]() mutable {
        bridge.reset();
    });
    worker.join();

    REQUIRE(waitForClose(probe));
    std::lock_guard<std::mutex> lock(probe->mutex);
    CHECK(probe->closeCount == 1);
    CHECK(probe->writeThread == loopThread);
    CHECK(probe->closeThread == loopThread);
}

DROGON_TEST(IoLoopResponseStream_DoesNotReenableWriteAfterQueuedDisconnect)
{
    auto *loop = drogon::app().getLoop();
    REQUIRE(loop != nullptr);

    std::promise<void> blockerEntered;
    auto blockerEnteredFuture = blockerEntered.get_future();
    std::promise<void> releaseBlocker;
    auto releaseFuture = releaseBlocker.get_future().share();
    loop->queueInLoop([&blockerEntered, releaseFuture]() mutable {
        blockerEntered.set_value();
        releaseFuture.wait();
    });
    REQUIRE(blockerEnteredFuture.wait_for(2s) == std::future_status::ready);

    auto probe = std::make_shared<DisconnectRaceProbe>(loop);
    loop->queueInLoop([probe]() {
        std::lock_guard<std::mutex> lock(probe->mutex);
        probe->connected = false;
        probe->writeEventEnabled = false;
    });

    auto responseStream = std::make_unique<drogon::ResponseStream>(
        std::make_unique<DisconnectRaceAsyncStream>(probe));
    auto bridge = IoLoopResponseStream::create(std::move(responseStream), loop);
    REQUIRE(bridge != nullptr);

    std::thread worker([bridge]() {
        bridge->send("late SSE payload");
        bridge->close();
    });
    worker.join();

    std::promise<void> drained;
    auto drainedFuture = drained.get_future();
    loop->queueInLoop([&drained]() { drained.set_value(); });
    releaseBlocker.set_value();
    REQUIRE(drainedFuture.wait_for(2s) == std::future_status::ready);

    std::lock_guard<std::mutex> lock(probe->mutex);
    CHECK(!probe->connected);
    CHECK(!probe->writeEventEnabled);
    CHECK(probe->rejectedSends >= 1);
}
