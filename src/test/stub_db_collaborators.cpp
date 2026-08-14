// R4 试点 C 配套：协作者链接桩。
//
// 定位必须说清，否则后人会误用：
// 本文件桩掉的是「被测单元的协作者」，不是被测单元本身。
// aiapi_test 现在链接真实的 accountManager.cpp（被测逻辑是真身编译出来的那份），
// 但 AccountManager 内部还会触达 ConfigDbManager / ChannelDbManager /
// RetoolWorkspaceService 等真实数据库协作者。
// 单元测试不应连真实数据库，故在链接期用空实现顶掉它们。
//
// 与「桩掉被测对象」的本质区别：若 AccountManager 的 store 注入逻辑被改坏，
// 测试会红；而旧的 stub_account_manager.cpp 做法下改坏真身测试照样绿。
//
// 缺口清单来自链接实测而非推测；Provider 退役后已同步删除不再需要的 backup 桩。
#include <dbManager/config/ConfigDbManager.h>
#include <dbManager/chaynsThread/chaynsThreadDbManager.h>
#include <test/chayns_thread_stub_control.h>
#include <retoolWorkspace/RetoolWorkspaceService.h>

#include <map>
#include <optional>
#include <string>

// ---- ConfigDbManager（5 个）----
void ConfigDbManager::initialize() {}
void ConfigDbManager::detectDbType() {}
bool ConfigDbManager::ensureTable(std::string*) { return true; }
std::optional<std::string> ConfigDbManager::getValue(const std::string&, std::string*)
{
    return std::nullopt;  // 测试里一律视为「配置项不存在」，走默认值分支
}
bool ConfigDbManager::setValues(const std::map<std::string, std::string>&, std::string*)
{
    return true;
}

// ---- ChannelDbManager ----
// 不在此桩接。步骤 82 查明：accountManager.cpp 内联展开 ChannelDbManager::getInstance()
// 时需要 vtable，而 vtable 只在 key function（第一个非内联虚函数）所在 TU 发射。
// 用标准 C++ 无法伪造 vtable，因此从生产库普通链接真实的
// channelDbManager.cpp object。它仅提供符号；测试用例不调用其任何方法，
// 故不会触达真实数据库。

// ---- chaynsThreadDbManager ----
// 测试二进制替换的是显式构造 ledger 的 DB 副作用：真实实现（走 drogon
// DbClient）不进 aiapi_test。桩原先是纯静默的（loadThreadsOlderThan 恒返回空），导致 runOnce()
// 的删除循环在单测中不可达——D4 的 M2 变异（限速等待退回 sleep_for）
// 因此无法被任何单测杀死。
//
// 现改为可控桩：测试注入待回收行，桩记录被调用的删除/重试序列。
// 只影响测试二进制，生产 composition root 仍注入真实 ledger。
void chaynsThreadDbManager::asyncUpsertThread(const ThreadRow&) {}
void chaynsThreadDbManager::asyncDetachThreadBySessionId(const std::string&) {}
void chaynsThreadDbManager::asyncUpdateThreadSessionId(const std::string&, const std::string&) {}
int chaynsThreadDbManager::purgeExhaustedThreads(int, std::string*) { return 0; }
std::vector<chaynsThreadDbManager::ThreadRow>
chaynsThreadDbManager::loadThreadsOlderThan(int64_t, int limit, std::string*)
{
    auto rows = chaynsThreadStubControl::instance().takeRows();
    if (limit > 0 && static_cast<int>(rows.size()) > limit) {
        rows.resize(static_cast<size_t>(limit));
    }
    return rows;
}
bool chaynsThreadDbManager::deleteThread(const std::string& threadId, std::string*)
{
    chaynsThreadStubControl::instance().recordDeleted(threadId);
    return true;
}
int chaynsThreadDbManager::bumpDeleteAttempts(const std::string& threadId, std::string*)
{
    chaynsThreadStubControl::instance().recordBumped(threadId);
    return 1;
}

// ---- chaynsThreadStubControl 实现 ----
chaynsThreadStubControl& chaynsThreadStubControl::instance()
{
    static chaynsThreadStubControl control;
    return control;
}
void chaynsThreadStubControl::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    rows_.clear();
    deleted_.clear();
    bumped_.clear();
    loadCalls_ = 0;
}
void chaynsThreadStubControl::setRows(std::vector<chaynsThreadDbManager::ThreadRow> newRows)
{
    std::lock_guard<std::mutex> lock(mutex_);
    rows_ = std::move(newRows);
}
std::vector<chaynsThreadDbManager::ThreadRow> chaynsThreadStubControl::takeRows()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++loadCalls_;
    auto out = rows_;
    rows_.clear();
    return out;
}
void chaynsThreadStubControl::recordDeleted(const std::string& threadId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    deleted_.push_back(threadId);
}
void chaynsThreadStubControl::recordBumped(const std::string& threadId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    bumped_.push_back(threadId);
}
std::vector<std::string> chaynsThreadStubControl::deletedIds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return deleted_;
}
std::vector<std::string> chaynsThreadStubControl::bumpedIds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bumped_;
}
int chaynsThreadStubControl::loadCallCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return loadCalls_;
}
