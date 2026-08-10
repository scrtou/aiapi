#pragma once

#include <runtime/StartupResult.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace lifecycle {

/**
 * @brief 一个具名的启动步骤。
 *
 * 之所以把步骤建模成数据而不是 `build()` 里的一串直写语句：G5 要求失败时能
 * 逆序 teardown 已完成的部分，而「已完成到第几步」只有在步骤可枚举时才是
 * 可判定的。名字进日志与失败 detail，使运维能直接定位到是哪一步崩的。
 */
struct StartupStep
{
    std::string                     name;
    std::function<StartupResult()>  run;
};

/**
 * @brief 一个后台 owner 的停机动作。
 *
 * `name` 不是装饰：停机日志此前是四条硬编码的 LOG_INFO，owner 一旦增删就与
 * 实际行为脱节。改成数据后日志由名字生成，不存在「说停了其实没停」的偏差。
 */
struct RuntimeOwner
{
    std::string           name;
    std::function<void()> stop;
};

/**
 * @brief composition root 的持有者。
 *
 * 现状（P4-W2 之前）：`main.cc` 同时是配置加载器、CORS 装配点、12+ 个单例的
 * 注入器和 4 个后台 owner 的停机编排者，且初始化横跨 main 线程 → event loop
 * → 队列 worker 三个上下文（G2），`run()` 开始收请求时初始化未必完成。
 *
 * AppContext 把这三件事收敛成一条线性时序：
 *
 *     配置 → build()  → run() → shutdown(deadline)
 *
 * build() 在**调用线程**上同步跑完全部步骤后才返回，因此 `run()` 之前的
 * happens-before 是构造性的，不再依赖「队列大概已经跑完了」这类假设。
 *
 * 本类不是单例：它由 main 在栈上持有，析构顺序因此是确定的（G7）。
 */
class AppContext
{
public:
    AppContext() = default;
    ~AppContext() = default;

    AppContext(const AppContext&)            = delete;
    AppContext& operator=(const AppContext&) = delete;
    AppContext(AppContext&&)                 = delete;
    AppContext& operator=(AppContext&&)      = delete;

    /// 注册一个启动步骤。必须在 build() 之前调用；顺序即执行顺序。
    void addStep(std::string name, std::function<StartupResult()> run);

    /**
     * 注册一个后台 owner 的停机动作。
     *
     * 约定：谁启动谁登记，且**紧接启动之后**登记——这样 build() 中途失败时，
     * ownersStarted() 恰好等于「已经起来的那些」，逆序 stop 才是正确的回滚。
     */
    void addOwner(std::string name, std::function<void()> stop);

    /**
     * 依次执行已注册步骤。
     *
     * - 步骤返回 Failed：立即停止推进，按登记逆序停掉已注册 owner，返回该失败。
     * - 步骤返回 Degraded：记入 degradedReasons() 并继续（G8 的两处有意降级）。
     * - 全部走完：Ok，或在存在降级时返回 Degraded 汇总。
     *
     * 重复调用返回 `AlreadyBuilt`——这是编程错误，不是环境错误，不应被重试逻辑吞掉。
     */
    [[nodiscard]] StartupResult build();

    /**
     * 按登记逆序停止全部 owner，deadline 为**绝对**时间点。
     *
     * 用绝对而非相对时长：停机要经过多个 owner，若每个各拿一份相对超时，总时长
     * 是各段之和，SIGTERM 宽限期会被逐段累加突破。绝对 deadline 让「还剩多少」
     * 在任意一段里都能直接算出。
     *
     * 幂等：二次调用为 no-op（G6）。
     */
    void shutdown(std::chrono::steady_clock::time_point deadline);

    bool isBuilt() const { return built_; }
    bool isShutdown() const { return shutdownDone_; }

    /// build() 期间记录的降级原因，供启动日志与 /ready 汇报使用。
    const std::vector<std::string>& degradedReasons() const { return degradedReasons_; }

    /// 已成功登记（即已启动）的 owner 数量，供回滚断言使用。
    size_t ownersStarted() const { return owners_.size(); }

    /// 已执行完成的步骤数量，失败时即为「失败步骤的下标」。
    size_t stepsCompleted() const { return stepsCompleted_; }

private:
    void stopOwnersInReverse(std::chrono::steady_clock::time_point deadline);

    std::vector<StartupStep>  steps_;
    std::vector<RuntimeOwner> owners_;
    std::vector<std::string>  degradedReasons_;
    size_t                    stepsCompleted_ = 0;
    bool                      built_          = false;
    bool                      shutdownDone_   = false;
};

}  // namespace lifecycle
