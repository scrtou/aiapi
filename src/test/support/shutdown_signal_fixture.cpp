#include <drogon/drogon.h>

#include <platform/ThreadJoin.h>
#include <runtime/AppContext.h>
#include <infrastructure/executor/BackgroundTaskQueue.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace
{
using namespace std::chrono_literals;

struct Scenario
{
    explicit Scenario(std::string value) : mode(std::move(value)) {}

    std::string mode;
    std::thread worker;
    platform::ThreadCompletionPtr done;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;

    void start()
    {
        if (mode == "idle" || mode == "backlog") return;

        done = std::make_shared<platform::ThreadCompletion>();
        worker = std::thread([this, completion = done] {
            struct Signaler
            {
                platform::ThreadCompletionPtr completion;
                ~Signaler() { completion->signal(); }
            } signaler{completion};

            std::unique_lock<std::mutex> lock(mutex);
            if (mode == "http") {
                // 模拟已经发出的、不可撤回的 HTTP 请求。deadline 取 200ms，
                // 故停机侧必须走 joinUntil 超时返回，再在进程退出前二次收割。
                lock.unlock();
                std::this_thread::sleep_for(2s);
                std::cout << "HTTP_COMPLETED" << std::endl;
                return;
            }
            if (mode == "polling") {
                cv.wait_for(lock, 10s, [this] { return stop; });
                if (stop) std::cout << "POLL_INTERRUPTED" << std::endl;
                return;
            }
            // disconnect：模拟客户端断连后，生成 worker 等待取消通知。
            cv.wait(lock, [this] { return stop; });
            std::cout << "DISCONNECT_INTERRUPTED" << std::endl;
        });
    }

    void requestStop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        cv.notify_all();
    }

    bool stopWithin(std::chrono::steady_clock::time_point deadline)
    {
        requestStop();
        if (!worker.joinable()) return true;
        if (!platform::joinUntil(worker, *done, deadline)) {
            std::cout << "SCENARIO_TIMEOUT" << std::endl;
            return false;
        }
        std::cout << "SCENARIO_STOPPED" << std::endl;
        return true;
    }

    void reap()
    {
        if (worker.joinable()) worker.join();
    }
};

}  // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "idle";
    if (mode != "idle" && mode != "http" && mode != "polling" &&
        mode != "backlog" && mode != "disconnect") {
        std::cerr << "unknown shutdown fixture mode: " << mode << std::endl;
        return 2;
    }

    std::cout << "MODE " << mode << std::endl;
    Scenario scenario(mode);
    scenario.start();

    auto queue = std::make_shared<BackgroundTaskQueue>();
    if (mode == "backlog") {
        queue->start(1);
        for (int i = 0; i < 4; ++i) {
            (void)queue->enqueue("fixture-backlog-" + std::to_string(i), [] {
                std::this_thread::sleep_for(100ms);
            });
        }
    }

    lifecycle::AppContext appContext;
    // 注册顺序模拟生产 composition root：队列、会话、账号、Reaper；
    // shutdown() 逆序执行，因此公共标记仍是 REAPER → ACCOUNTS → SESSION → QUEUE。
    if (mode != "idle") {
        appContext.addOwner("scenario worker", [&scenario](auto deadline) {
            (void)scenario.stopWithin(deadline);
        });
    }
    appContext.addOwner("task-queue", [mode, queue](auto deadline) {
        if (mode == "backlog") {
            if (queue->shutdown(deadline)) std::cout << "BACKLOG_DRAINED" << std::endl;
            else std::cout << "BACKLOG_TIMEOUT" << std::endl;
        }
        std::cout << "QUEUE" << std::endl;
    });
    appContext.addOwner("session-cleaner", [](auto) { std::cout << "SESSION" << std::endl; });
    appContext.addOwner("account-workers", [](auto) { std::cout << "ACCOUNTS" << std::endl; });
    appContext.addOwner("chayns-reaper", [](auto) { std::cout << "REAPER" << std::endl; });

    drogon::app().getLoop()->runAfter(0.05, [mode] {
        std::cout << "READY" << std::endl;
        std::cout.flush();
        // The signal is sent by the parent harness after READY. Keep mode alive
        // until SIGTERM rather than exiting the event loop on its own.
        (void)mode;
    });

    drogon::app().run();

    const auto budget = mode == "http" ? 200ms : 5s;
    appContext.shutdown(std::chrono::steady_clock::now() + budget);
    scenario.reap();
    std::cout << "EXIT" << std::endl;
    return 0;
}
