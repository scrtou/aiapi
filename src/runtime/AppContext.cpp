#include <runtime/AppContext.h>

#include <application/account/accountManager.h>
#include <application/channel/channelManager.h>
#include <infrastructure/persistence/account/accountBackupDbManager.h>
#include <infrastructure/persistence/account/accountDbManager.h>
#include <infrastructure/persistence/channel/channelDbManager.h>
#include <infrastructure/persistence/chaynsThread/chaynsThreadDbManager.h>
#include <infrastructure/persistence/config/ConfigDbManager.h>
#include <infrastructure/persistence/metrics/ErrorStatsDbManager.h>
#include <infrastructure/persistence/metrics/StatusDbManager.h>
#include <infrastructure/persistence/retoolWorkspace/RetoolWorkspaceDbManager.h>
#include <infrastructure/persistence/session/SessionDbManager.h>
#include <infrastructure/provider/chayns/chaynsThreadReaper.h>
#include <drogon/drogon.h>
#include <infrastructure/metrics/ErrorStatsService.h>
#include <application/workspace/RetoolWorkspaceManager.h>
#include <application/generation/core/Session.h>
#include <infrastructure/executor/BackgroundTaskQueue.h>

#include <utility>

namespace lifecycle {

AppContext::~AppContext() = default;

IBackgroundExecutor* AppContext::backgroundExecutor() const
{
    return backgroundTaskQueue_.get();
}

void AppContext::addStep(std::string name, std::function<StartupResult()> run)
{
    steps_.push_back(StartupStep{std::move(name), std::move(run)});
}

void AppContext::addOwner(std::string name,
                          std::function<void(std::chrono::steady_clock::time_point)> stop)
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
        const auto startedAt = std::chrono::steady_clock::now();
        it->stop(deadline);
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt);
        // 逐 owner 记录实耗：H3 的现场特征是「总时长超支但看不出是谁吃掉的」。
        // 没有这条日志，超支只能靠通读五个 owner 的实现去猜。
        LOG_INFO << "[停机] " << it->name << " 已停止，耗时 " << spent.count() << "ms";
    }
    owners_.clear();
}

}  // namespace lifecycle
