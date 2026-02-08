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
    static BackgroundTaskQueue& instance()
    {
        static BackgroundTaskQueue inst;
        return inst;
    }

    // / 初始化工作线程（如未手动调用，en队列 时会自动初始化）
    void start(size_t numThreads = 2)
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
    void enqueue(const std::string& name, std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!started_) {
                // 自动启动
                started_ = true;
                stopping_ = false;
                for (size_t i = 0; i < 2; ++i) {
                    workers_.emplace_back([this, i]() { workerLoop(i); });
                }
                LOG_INFO << "[后台任务队列] 自动启动 2 个工作线程";
            }
            if (stopping_) {
                LOG_WARN << "[后台任务队列] 已停机，忽略任务：" << name;
                return;
            }
            tasks_.push({name, std::move(task)});
        }
        cv_.notify_one();
        LOG_DEBUG << "[后台任务队列] 任务入队：" << name;
    }

    // / 优雅停机：等待队列排空，然后 所有线程
    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
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
        LOG_DEBUG << "[后台任务队列] 工作线程 #" << id << " 已启动";
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
                LOG_DEBUG << "[后台任务队列] 线程 #" << id
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
        LOG_DEBUG << "[后台任务队列] 工作线程 #" << id << " 已退出";
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::queue<NamedTask> tasks_;
    std::vector<std::thread> workers_;
    bool started_ = false;
    bool stopping_ = false;
};
