#pragma once

#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <memory>
#include <drogon/drogon.h>

/**
 * @brief 后台任务执行器的入队结果。
 *
 * 旧接口返回裸 bool，调用方无法区分「队列满」与「已停机」——前者应让客户端
 * 稍后重试，后者是终态。P4-W1 起用四态区分，[[nodiscard]] 强制处理。
 */
enum class EnqueueResult
{
    Accepted,      ///< 已入队，必定会被执行或在 drain 中执行完
    QueueFull,     ///< 达到容量上限，背压；调用方应重试或降级
    ShuttingDown,  ///< 正在 drain，拒绝一切新任务
    Stopped        ///< 终态或尚未 start()，不可逆
};

inline const char* toString(EnqueueResult r)
{
    switch (r) {
        case EnqueueResult::Accepted:     return "Accepted";
        case EnqueueResult::QueueFull:    return "QueueFull";
        case EnqueueResult::ShuttingDown: return "ShuttingDown";
        case EnqueueResult::Stopped:      return "Stopped";
    }
    return "Unknown";
}

/**
 * @brief 后台任务队列 - 替换裸 std::thread(...).detach()
 *
 * 固定数量工作线程消费任务，支持优雅停机（drain + join）。
 *
 * 用法:
 *   auto r = BackgroundTaskQueue::instance().enqueue("taskName", []{});
 *   调用方必须处理 r != EnqueueResult::Accepted 的情形。
 *
 * 停机:
 *   BackgroundTaskQueue::instance().shutdown();  // drain 后 join 所有线程
 */
class BackgroundTaskQueue
{
public:
    static constexpr size_t kDefaultWorkerThreads = 8;

    /// 队列容量上限。超过即返回 QueueFull，把无界增长换成显式背压信号。
    /// 取值依据：8 线程 x 单任务典型耗时（DB upsert 量级 ~10ms）下，1024 条
    /// 积压对应约 1.3s 排空时间，仍在 SIGTERM drain 窗口内；再大则停机超时。
    static constexpr size_t kDefaultCapacity = 1024;

    /**
     * 单向状态机，无回边：
     *   Fresh --start()--> Running --shutdown()--> Draining --(排空)--> Stopped
     * 这是 N2「线程池不可复活」不变量的类型化表达；旧实现用
     * started_/stopping_/shutdownCalled_ 三个 bool 表示，8 种组合里只有 4 种
     * 合法，非法组合仅靠注释约束。
     */
    enum class State { Fresh, Running, Draining, Stopped };

    static BackgroundTaskQueue& instance()
    {
        static BackgroundTaskQueue inst;
        return inst;
    }

    /// 初始化工作线程（如未手动调用，enqueue 时会自动初始化）
    void start(size_t numThreads = kDefaultWorkerThreads)
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Draining/Stopped 之后不得重新启动——这正是 N2 修复的不变量。
        if (state_ != State::Fresh) return;
        state_ = State::Running;
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this, i]() { workerLoop(i); });
        }
        LOG_INFO << "[后台任务队列] 启动" << numThreads << " 个工作线程";
    }

    /// 提交任务到队列。调用方必须处理返回值。
    [[nodiscard]] EnqueueResult enqueue(const std::string& name,
                                        std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);

            // N2 修复保留：停机判断必须【前置】于自动启动分支。
            // 旧实现把 `if (!started_)` 放在前面，而 shutdown() 结尾会置
            // started_=false，于是一个迟到的 enqueue 会重新 spawn 8 条线程
            // 并把 stopping_ 复位，导致线程池复活、进程永不退出。
            // 四态状态机把该不变量变成了「无回边」的结构性保证。
            if (state_ == State::Draining) {
                LOG_WARN << "[后台任务队列] 正在停机，拒绝任务：" << name;
                return EnqueueResult::ShuttingDown;
            }
            if (state_ == State::Stopped) {
                LOG_WARN << "[后台任务队列] 已停机，拒绝任务：" << name;
                return EnqueueResult::Stopped;
            }

            // 容量上限先于自动启动分支判断：一个注定被拒的任务不应该
            // 反过来触发 8 条工作线程的 spawn。
            if (tasks_.size() >= capacity_) {
                LOG_WARN << "[后台任务队列] 队列已满(" << capacity_
                         << ")，背压拒绝任务：" << name;
                return EnqueueResult::QueueFull;
            }

            if (state_ == State::Fresh) {
                state_ = State::Running;
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
        return EnqueueResult::Accepted;
    }

    /// 优雅停机：拒绝新任务，等待队列排空，然后 join 所有线程
    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            // Fresh 直接落到 Stopped：即使队列从未启动过，也要封死后续
            // enqueue，否则「启动后未收到任何请求就退出」的场景仍可被复活。
            if (state_ == State::Fresh) {
                state_ = State::Stopped;
                return;
            }
            if (state_ != State::Running) return;  // Draining/Stopped 幂等
            state_ = State::Draining;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
        workers_.clear();
        {
            std::lock_guard<std::mutex> lk(mu_);
            state_ = State::Stopped;
        }
        LOG_INFO << "[后台任务队列] 已停机，所有工作线程已退出";
    }

    /**
     * 等待队列排空（待处理为 0 且无任务在执行），或直到 deadline 到期。
     *
     * P4-W2 的 AppContext 需要「drain 完成」这一事件，而不是只能盲目 join。
     * 与 shutdown() 的关键区别：本方法不改变状态机，可在 Running 下调用；
     * 它只观测，不推进。
     *
     * 空闲判定必须同时满足 tasks_ 为空【与】running_ 为 0：只看队列会在
     * 「最后一个任务已出队、仍在执行」的窗口内误报空闲，从而让 AppContext
     * 提前认为可以销毁依赖，正是这类竞态导致停机期 use-after-free。
     *
     * @return true 表示已达空闲；false 表示 deadline 到期时仍有未完成工作。
     */
    bool waitUntilIdle(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mu_);
        return idleCv_.wait_for(lk, timeout, [this]() {
            return tasks_.empty() && running_ == 0;
        });
    }

    /// 无超时版本：一直等到空闲。仅供已确知不会再有新任务的场景使用。
    void waitUntilIdle()
    {
        std::unique_lock<std::mutex> lk(mu_);
        idleCv_.wait(lk, [this]() { return tasks_.empty() && running_ == 0; });
    }

    /// 当前正在执行（已出队但未完成）的任务数。
    size_t runningCount() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return running_;
    }

    /// 队列容量上限（构造期固定，运行期不可变——避免停机中途放宽背压）
    size_t capacity() const { return capacity_; }

    /// 已 spawn 的工作线程数。用于验证「被拒的入队不得反向触发 spawn」。
    size_t workerCount() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return workers_.size();
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
        return state_ == State::Fresh || state_ == State::Running;
    }

    ~BackgroundTaskQueue()
    {
        shutdown();
    }

    /**
     * 测试专用工厂：返回一个独立实例，不复用进程级单例。
     *
     * 单例的状态机是单向不可逆的（Fresh->Running->Draining->Stopped），
     * 这正是 N2 要保证的不变量；因此任何调用 shutdown() 的用例都会永久
     * 污染同进程内后续用例。测试必须各自持有独立实例，而不是靠
     * 「shutdown 后能重新 start」来复位——那恰恰是被修复掉的缺陷。
     */
    static std::unique_ptr<BackgroundTaskQueue> createForTesting(
        size_t capacity = kDefaultCapacity)
    {
        return std::unique_ptr<BackgroundTaskQueue>(
            new BackgroundTaskQueue(capacity));
    }

    // 禁止拷贝
    BackgroundTaskQueue(const BackgroundTaskQueue&) = delete;
    BackgroundTaskQueue& operator=(const BackgroundTaskQueue&) = delete;

private:
    explicit BackgroundTaskQueue(size_t capacity = kDefaultCapacity)
        : capacity_(capacity) {}

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
                cv_.wait(lk, [this]() {
                    return state_ != State::Running || !tasks_.empty();
                });
                if (state_ != State::Running && tasks_.empty()) {
                    break;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
                ++running_;  // 持锁期间自增，与出队同为一个原子步骤
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
            // 无论成功还是抛异常都必须递减并通知，否则一次异常就会让
            // waitUntilIdle 永久挂起——因此放在 try/catch 之外。
            {
                std::lock_guard<std::mutex> lk(mu_);
                --running_;
                if (tasks_.empty() && running_ == 0) {
                    idleCv_.notify_all();
                }
            }
        }
        LOG_INFO << "[后台任务队列] 工作线程 #" << id << " 已退出";
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;      ///< 唤醒 worker 取任务
    std::condition_variable idleCv_;  ///< 唤醒 waitUntilIdle 观测方
    std::queue<NamedTask> tasks_;
    std::vector<std::thread> workers_;
    size_t running_ = 0;           ///< 已出队但未执行完的任务数
    State state_ = State::Fresh;   // N2：单向不可逆，无回边
    const size_t capacity_ = kDefaultCapacity;
};
