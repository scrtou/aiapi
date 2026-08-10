#include <runtime/AppContext.h>

#include <drogon/drogon.h>

#include <utility>

namespace lifecycle {

void AppContext::addStep(std::string name, std::function<StartupResult()> run)
{
    steps_.push_back(StartupStep{std::move(name), std::move(run)});
}

void AppContext::addOwner(std::string name, std::function<void()> stop)
{
    owners_.push_back(RuntimeOwner{std::move(name), std::move(stop)});
}

StartupResult AppContext::build()
{
    if (built_) {
        return StartupResult::failed(StartupError::AlreadyBuilt,
                                     "AppContext::build() called more than once");
    }

    for (const auto& step : steps_) {
        if (!step.run) {
            // 空步骤说明装配代码漏填了函数体。放行会让后续步骤在缺少前置的
            // 状态下运行，正是 G3 那类「隐式顺序约束被打破却不报错」的场景。
            const auto result = StartupResult::failed(
                StartupError::OwnerStartFailed, "startup step has no callable: " + step.name);
            LOG_FATAL << "[启动] 步骤 " << step.name << " 未提供实现";
            stopOwnersInReverse(std::chrono::steady_clock::now() + std::chrono::seconds(5));
            return result;
        }

        const auto result = step.run();

        if (result.isFailed()) {
            LOG_FATAL << "[启动] 步骤 " << step.name << " 失败: " << toString(result.error())
                      << " - " << result.detail();
            // 回滚用 5s 兜底 deadline：此时进程尚未对外服务，没有 SIGTERM 宽限期
            // 的约束，但也不能无限等待一个卡住的 owner。
            stopOwnersInReverse(std::chrono::steady_clock::now() + std::chrono::seconds(5));
            return result;
        }

        if (result.isDegraded()) {
            LOG_WARN << "[启动] 步骤 " << step.name << " 降级: " << result.detail();
            degradedReasons_.push_back(step.name + ": " + result.detail());
        }

        ++stepsCompleted_;
    }

    built_ = true;

    if (!degradedReasons_.empty()) {
        return StartupResult::degraded("startup completed with " +
                                       std::to_string(degradedReasons_.size()) +
                                       " degraded step(s)");
    }
    return StartupResult::ok();
}

void AppContext::shutdown(std::chrono::steady_clock::time_point deadline)
{
    if (shutdownDone_) {
        LOG_INFO << "[停机] shutdown() 重复调用，忽略";
        return;
    }
    shutdownDone_ = true;
    stopOwnersInReverse(deadline);
}

void AppContext::stopOwnersInReverse(std::chrono::steady_clock::time_point deadline)
{
    for (auto it = owners_.rbegin(); it != owners_.rend(); ++it) {
        if (!it->stop) continue;

        // deadline 已过并不跳过 stop：owner 的 stop() 多为 join，跳过会把线程
        // 留到进程退出时被强行截断，正是 N4 修掉的那类问题。这里只如实记录
        // 超支，真正的取消能力在各 owner 内部（C6 逐个补齐）。
        if (std::chrono::steady_clock::now() >= deadline) {
            LOG_WARN << "[停机] 已超过 deadline，仍将停止 owner: " << it->name;
        }

        LOG_INFO << "[停机] 正在停止 " << it->name << "...";
        it->stop();
        LOG_INFO << "[停机] " << it->name << " 已停止";
    }
    owners_.clear();
}

}  // namespace lifecycle
