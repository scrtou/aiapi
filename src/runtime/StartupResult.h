#pragma once

#include <string>
#include <utility>

namespace lifecycle {

/**
 * @brief 启动阶段的失败原因码。
 *
 * 动因（P4-W2 / G1、G4）：当前 composition root 的启动步骤几乎全是无返回值的
 * `init()`，唯一一处 `return 1` 还位于 `queueInLoop` 投递的 lambda 内——
 * trantor 的 `queueInLoop(Func&&)` 形参是 `std::function<void()>`，返回值被直接
 * 丢弃。于是「后台初始化任务入队失败」打完 LOG_FATAL 之后进程照常运行，
 * 恰好落入注释自己所担心的「未初始化 Store / 未建表的半启动进程」。
 *
 * 原因码的粒度按「运维看到日志后要做的动作」划分，而不是按抛出点划分：
 * 同样是建表失败，会话快照表失败可以继续跑（降级纯内存），而账号 Store
 * 缺失则必须终止——两者若共用一个码，回滚策略就无法从码本身推导。
 */
enum class StartupError
{
    None,             ///< 占位：Ok / Degraded 时取此值，便于无分支打印
    ConfigInvalid,    ///< 配置文件自检未过（main.cc 现有 validateConfigFile 分支）
    ExecutorRejected, ///< 后台执行器拒收初始化任务；即 G1，此前被静默吞掉
    StoreInitFailed,  ///< 必需的 Store/DbManager 初始化失败，无降级路径
    OwnerStartFailed, ///< 某个后台 owner（线程/定时器）启动失败
    AlreadyBuilt      ///< 重复 build()，属编程错误而非环境错误
};

inline const char* toString(StartupError e)
{
    switch (e) {
        case StartupError::None:             return "None";
        case StartupError::ConfigInvalid:    return "ConfigInvalid";
        case StartupError::ExecutorRejected: return "ExecutorRejected";
        case StartupError::StoreInitFailed:  return "StoreInitFailed";
        case StartupError::OwnerStartFailed: return "OwnerStartFailed";
        case StartupError::AlreadyBuilt:     return "AlreadyBuilt";
    }
    return "Unknown";
}

/**
 * @brief 启动步骤的三态结果。
 *
 * 三态而非二态的理由是 G8：`main.cc` 里有两处「建表失败 → 静默降级」是
 * **有意为之**的产品行为——会话持久化表建不出来时退回纯内存会话，chayns
 * 线程台账建不出来时关闭回收器。如果只有 Ok/Failed，迁移时这两处要么被
 * fail-fast 误伤（本可服务的进程拒绝启动），要么退回 `bool` 并丢失原因。
 *
 * Degraded 与 Ok 一样允许继续启动，区别在于它携带 detail 且必须被日志
 * 显式记录，使「进程活着但功能少一块」这件事在启动日志里可检索。
 */
class StartupResult
{
public:
    static StartupResult ok() { return StartupResult(State::Ok, StartupError::None, {}); }

    static StartupResult degraded(std::string detail)
    {
        return StartupResult(State::Degraded, StartupError::None, std::move(detail));
    }

    static StartupResult failed(StartupError code, std::string detail)
    {
        return StartupResult(State::Failed, code, std::move(detail));
    }

    /// 是否允许继续走后续启动步骤。Degraded 为真——这正是它存在的意义。
    bool canProceed() const { return state_ != State::Failed; }
    bool isOk() const { return state_ == State::Ok; }
    bool isDegraded() const { return state_ == State::Degraded; }
    bool isFailed() const { return state_ == State::Failed; }

    StartupError error() const { return code_; }
    const std::string& detail() const { return detail_; }

    const char* stateName() const
    {
        switch (state_) {
            case State::Ok:       return "Ok";
            case State::Degraded: return "Degraded";
            case State::Failed:   return "Failed";
        }
        return "Unknown";
    }

private:
    enum class State { Ok, Degraded, Failed };

    StartupResult(State s, StartupError c, std::string d)
        : state_(s), code_(c), detail_(std::move(d))
    {
    }

    State        state_;
    StartupError code_;
    std::string  detail_;
};

}  // namespace lifecycle
