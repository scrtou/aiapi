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
#include <optional>
#include <drogon/drogon.h>

#include <platform/ThreadJoin.h>

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
            // 每条 worker 独占一个完成标志。ThreadCompletion 是一次性信号，
            // 不可在多条线程间共享——共享会让第一条退出就宣告全体完成，
            // 限时汇合随即静默退化成无限 join。
            auto done = std::make_shared<platform::ThreadCompletion>();
            workerDone_.push_back(done);
            workers_.emplace_back([this, i, done]() {
                // 析构守卫：正常 return、break 还是异常展开，都必须自报完成。
                // 漏报会让停机侧白等满预算，把「线程早已退出」误判为超支。
                struct Signal {
                    platform::ThreadCompletionPtr d;
                    ~Signal() { d->signal(); }
                } guard{done};
                workerLoop(i);
            });
        }
        LOG_INFO << "[后台任务队列] 启动" << numThreads << " 个工作线程";
    }

    /// 提交任务到队列。调用方必须处理返回值。
    [[nodiscard]] EnqueueResult enqueue(const std::string& name,
                                        std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);

            // N2 修复保留：停机判断必须【前置】于其余一切分支。
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

            // C6：不再隐式 spawn 工作线程。启动时机归 composition root 所有
            // （main.cc 读取 background_task_threads 后显式 start()）。隐式启动
            // 会让任意一次早到的 enqueue 抢先用 kDefaultWorkerThreads 建池，
            // 使配置项静默失效，且线程池生命周期的起点无法被 AppContext 观测。
            //
            // 本分支必须【先于】容量判断：Fresh 队列没有 worker，永远不会
            // 自行排空；若返回 QueueFull，调用方会把「忘记 start()」这个编程
            // 错误误读为可重试的背压而无限重试。Stopped 是终态，才能
            // 正确表达「不要再试了，去修启动顺序」。
            if (state_ == State::Fresh) {
                LOG_ERROR << "[后台任务队列] 尚未 start()，拒绝任务：" << name;
                return EnqueueResult::Stopped;
            }

            // 背压：只在 Running 下有意义——此时 worker 正在消费，QueueFull
            // 确实是「稍后可重试」的瞬态拒绝。
            if (tasks_.size() >= capacity_) {
                LOG_WARN << "[后台任务队列] 队列已满(" << capacity_
                         << ")，背压拒绝任务：" << name;
                return EnqueueResult::QueueFull;
            }

            tasks_.push({name, std::move(task)});
        }
        cv_.notify_one();
        LOG_INFO << "[后台任务队列] 任务入队：" << name;
        return EnqueueResult::Accepted;
    }

    /// 优雅停机：拒绝新任务，等待队列排空，然后 join 所有线程。
    /// 语义与 P4-W1 起完全一致——无限等待，直到全部 worker 退出。
    /// 亦用于收割上一轮限时停机超预算后残留的线程。
    void shutdown() { (void)shutdownImpl(std::nullopt); }

    /**
     * 限时停机（P4-D5）：在绝对 deadline 之前完成 drain + join。
     *
     * @return true  全部 worker 已汇合，队列已进入 Stopped；
     *         false 预算耗尽仍有 worker 未退出。此时**状态停留在 Draining**，
     *               线程既未 join 也未 detach，仍被本对象持有。
     *
     * 为什么超预算时不清空 workers_：`std::vector<std::thread>` 析构会对
     * joinable 元素调用 std::terminate。「停机超时」绝不能升级成进程崩溃。
     * 也不推进到 Stopped——那会让残局无从辨认，调用方将失去再次收割的依据。
     *
     * 注意 deadline 是绝对时间点：N 条线程依次汇合共用同一个截止时间，
     * 总上界仍是 deadline 本身，而非 N 倍预算。
     */
    [[nodiscard]] bool shutdown(std::chrono::steady_clock::time_point deadline)
    {
        return shutdownImpl(deadline);
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

    bool shutdownImpl(std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            // Fresh 直接落到 Stopped：即使队列从未启动过，也要封死后续
            // enqueue，否则「启动后未收到任何请求就退出」的场景仍可被复活。
            if (state_ == State::Fresh) {
                state_ = State::Stopped;
                return true;
            }
            if (state_ == State::Stopped) return true;  // 终态，幂等
            // Running -> Draining；若已是 Draining，说明是在收割上一轮限时
            // 停机的残局，状态不必也不能回退。
            state_ = State::Draining;
        }
        cv_.notify_all();

        bool allJoined = true;
        for (size_t i = 0; i < workers_.size(); ++i) {
            auto& w = workers_[i];
            if (!w.joinable()) continue;
            if (deadline) {
                if (!platform::joinUntil(w, *workerDone_[i], *deadline)) {
                    allJoined = false;
                    // 不 break：后续线程可能已退出，能收一条是一条。
                    // 预算已过期时它们的 waitUntil 会立即返回，不会追加等待。
                }
            } else {
                w.join();
            }
        }

        if (!allJoined) {
            LOG_WARN << "[后台任务队列] 停机预算耗尽，仍有工作线程未退出；"
                     << "线程与队列保持原样，等待后续收割";
            return false;
        }

        workers_.clear();
        workerDone_.clear();
        {
            std::lock_guard<std::mutex> lk(mu_);
            state_ = State::Stopped;
        }
        LOG_INFO << "[后台任务队列] 已停机，所有工作线程已退出";
        return true;
    }

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
    /// 与 workers_ 同下标一一对应的完成标志，供限时汇合使用。
    std::vector<platform::ThreadCompletionPtr> workerDone_;
    size_t running_ = 0;           ///< 已出队但未执行完的任务数
    State state_ = State::Fresh;   // N2：单向不可逆，无回边
    const size_t capacity_ = kDefaultCapacity;
};
