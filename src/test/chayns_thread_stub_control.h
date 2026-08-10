#ifndef AIAPI_TEST_CHAYNS_THREAD_STUB_CONTROL_H
#define AIAPI_TEST_CHAYNS_THREAD_STUB_CONTROL_H

#include <dbManager/chaynsThread/chaynsThreadDbManager.h>

#include <mutex>
#include <string>
#include <vector>

/**
 * 测试专用控制块：chaynsThreadDbManager 在 aiapi_test 里由
 * stub_db_collaborators.cpp 做链接期整体替换（真实实现不进测试二进制）。
 * 该桩原先是纯静默的（loadThreadsOlderThan 恒返回空），导致 runOnce()
 * 的删除循环在单测中不可达 —— D4 的 M2 变异因此无法被杀死。
 *
 * 这里把桩改为可控：测试注入待回收行，桩记录被调用的删除/重试计数序列。
 * 只影响测试二进制，生产代码不受任何影响。
 */
struct chaynsThreadStubControl
{
    static chaynsThreadStubControl& instance();

    void reset();

    /// 测试注入：下一次 loadThreadsOlderThan 返回的行
    void setRows(std::vector<chaynsThreadDbManager::ThreadRow> newRows);

    /// 桩内部调用
    std::vector<chaynsThreadDbManager::ThreadRow> takeRows();
    void recordDeleted(const std::string& threadId);
    void recordBumped(const std::string& threadId);

    /// 断言用
    std::vector<std::string> deletedIds() const;
    std::vector<std::string> bumpedIds() const;
    int loadCallCount() const;

  private:
    mutable std::mutex mutex_;
    std::vector<chaynsThreadDbManager::ThreadRow> rows_;
    std::vector<std::string> deleted_;
    std::vector<std::string> bumped_;
    int loadCalls_ = 0;
};

#endif  // AIAPI_TEST_CHAYNS_THREAD_STUB_CONTROL_H
