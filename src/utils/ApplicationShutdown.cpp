#include <utils/ApplicationShutdown.h>

#include <drogon/drogon.h>

namespace lifecycle {
namespace {

void invoke(const std::function<void()>& action)
{
    if (action) action();
}

}  // namespace

void runApplicationShutdown(const ApplicationShutdownActions& actions)
{
    LOG_INFO << "[停机] 正在停止 chayns thread reaper...";
    invoke(actions.stopReaper);
    LOG_INFO << "[停机] chayns thread reaper 已停止";

    LOG_INFO << "[停机] 正在关闭账号管理器后台线程...";
    invoke(actions.stopAccountWorkers);
    LOG_INFO << "[停机] 账号管理器后台线程已关闭";

    LOG_INFO << "[停机] 正在停止会话过期清理线程...";
    invoke(actions.stopSessionCleaner);
    LOG_INFO << "[停机] 会话过期清理线程已停止";

    LOG_INFO << "[停机] 正在关闭后台任务队列...";
    invoke(actions.shutdownTaskQueue);
    LOG_INFO << "[停机] 后台任务队列已停机";
}

}  // namespace lifecycle
