#pragma once

#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <string>
#include <vector>
#include <drogon/drogon.h>

/**
 * @brief 后台任务队列 — 替换裸 std::thread(...).detach()
 *
 * 使用固定数量的工作线程从队列中消费任务，
 * 支持优雅停机（drain + join）。
 *
 * 用法:
 *   BackgroundTaskQueue::instance().enqueue("taskName", [](){ ... });
 *
 * 停机:
 *   BackgroundTaskQueue::instance().shutdown();  // 等待队列排空后 所有线程
 */
class BackgroundTaskQueue
{
public:
    static constexpr size_t kDefaultWorkerThreads = 8;

    static BackgroundTaskQueue& instance()
    {
        static BackgroundTaskQueue inst;
        return inst;
    }

    // / 初始化工作线程（如未手动调用，en队列 时会自动初始化）
    void start(size_t numThreads = kDefaultWorkerThreads)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (started_) return;
        started_ = true;
        stopping_ = false;
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this, i]() { workerLoop(i); });
        }
        LOG_INFO << "[后台任务队列] 启动" << numThreads << " 个工作线程";
    }

    /// 提交任务到队列
    bool enqueue(const std::string& name, std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);

            // N2 修复：停机判断必须【前置】于自动启动分支。
            // 旧实现把 `if (!started_)` 放在前面，而 shutdown() 结尾会置
            // started_=false，于是一个迟到的 enqueue 会重新 spawn 8 条线程
            // 并把 stopping_ 复位，导致后面的 `if (stopping_)` 永不生效。
            // shutdownCalled_ 是不可逆的一次性标志，杜绝线程池复活。
            if (stopping_ || shutdownCalled_) {
                LOG_WARN << "[后台任务队列] 已停机，拒绝任务：" << name;
                return false;
            }

            if (!started_) {
                started_ = true;
                for (size_t i = 0; i < kDefaultWorkerThreads; ++i) {
                    workers_.emplace_back([this, i]() { workerLoop(i); });
                }
                LOG_INFO << "[后台任务队列] 自动启动 "
                         << kDefaultWorkerThreads << " 个工作线程";
            }

            tasks_.push({name, std::move(task)});
        }
        cv_.notify_one();
        LOG_INFO << "[后台任务队列] 任务入队：" << name;
        return true;
    }
    // / 优雅停机：等待队列排空，然后 所有线程
    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            // 先置不可逆标志：即使队列从未启动过，也要封死后续 enqueue，
            // 否则「启动后未收到任何请求就退出」的场景仍可被复活。
            shutdownCalled_ = true;
            if (!started_ || stopping_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
        workers_.clear();
        started_ = false;
        LOG_INFO << "[后台任务队列] 已停机，所有工作线程已";
    }

    /// 返回当前队列中待处理的任务数
    size_t pendingCount() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return tasks_.size();
    }

    /// True until shutdown closes the mutex-protected enqueue boundary.
    bool acceptingTasks() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return !stopping_ && !shutdownCalled_;
    }

    ~BackgroundTaskQueue()
    {
        shutdown();
    }

    // 禁止拷贝
    BackgroundTaskQueue(const BackgroundTaskQueue&) = delete;
    BackgroundTaskQueue& operator=(const BackgroundTaskQueue&) = delete;

private:
    BackgroundTaskQueue() = default;

    struct NamedTask {
        std::string name;
        std::function<void()> fn;
    };

    void workerLoop(size_t id)
    {
        LOG_INFO << "[后台任务队列] 工作线程 #" << id << " 已启动";
        while (true) {
            NamedTask task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this]() { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) {
                    break;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            try {
                LOG_INFO << "[后台任务队列] 线程 #" << id
                          << " 执行任务: " << task.name;
                task.fn();
            } catch (const std::exception& e) {
                LOG_ERROR << "[后台任务队列] 任务 '" << task.name
                          << "' 异常: " << e.what();
            } catch (...) {
                LOG_ERROR << "[后台任务队列] 任务 '" << task.name
                          << "' 未知异常";
            }
        }
        LOG_INFO << "[后台任务队列] 工作线程 #" << id << " 已退出";
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::queue<NamedTask> tasks_;
    std::vector<std::thread> workers_;
    bool started_ = false;
    bool stopping_ = false;
    bool shutdownCalled_ = false;   // N2：不可逆停机标志
};
