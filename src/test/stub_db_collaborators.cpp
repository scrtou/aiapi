// R4 试点 C 配套：协作者链接桩。
//
// 定位必须说清，否则后人会误用：
// 本文件桩掉的是「被测单元的协作者」，不是被测单元本身。
// aiapi_test 现在链接真实的 accountManager.cpp（被测逻辑是真身编译出来的那份），
// 但 AccountManager 内部还会触达 ConfigDbManager / AccountBackupDbManager /
// ChannelDbManager / RetoolWorkspaceService 这四个真实数据库协作者。
// 单元测试不应连真实数据库，故在链接期用空实现顶掉它们。
//
// 与「桩掉被测对象」的本质区别：若 AccountManager 的 store 注入逻辑被改坏，
// 测试会红；而旧的 stub_account_manager.cpp 做法下改坏真身测试照样绿。
//
// 缺口清单来自实测（步骤 79）而非推测，共 10 个符号。
#include <dbManager/config/ConfigDbManager.h>
#include <dbManager/account/accountBackupDbManager.h>
#include <dbManager/chaynsThread/chaynsThreadDbManager.h>
#include <retoolWorkspace/RetoolWorkspaceService.h>

#include <map>
#include <optional>
#include <string>

// ---- ConfigDbManager（4 个）----
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

// ---- AccountBackupDbManager（3 个）----
AccountBackupDbManager::AccountBackupDbManager() {}
bool AccountBackupDbManager::ensureTable() { return true; }
bool AccountBackupDbManager::backupAccount(const Accountinfo_st&, const std::string&)
{
    return true;
}

// ---- ChannelDbManager ----
// 不在此桩接。步骤 82 查明：accountManager.cpp 内联展开 ChannelDbManager::getInstance()
// 时需要 vtable，而 vtable 只在 key function（第一个非内联虚函数）所在 TU 发射。
// 用标准 C++ 无法伪造 vtable，因此改为把真实的 channelDbManager.cpp 编进测试目标。
// 它仅提供符号；测试用例不调用其任何方法，故不会触达真实数据库。

// ---- RetoolWorkspaceService（1 个）----
RetoolWorkspaceInfo RetoolWorkspaceService::provisionWorkspace(const Json::Value&, std::string*)
{
    return RetoolWorkspaceInfo{};
}

// ---- ChaynsThreadDbManager ----
// Chayns provider characterization compiles the real provider but keeps its
// asynchronous ledger collaborator disabled.  The upstream HTTP transport is
// faked separately; neither database nor network is touched.
std::shared_ptr<chaynsThreadDbManager> chaynsThreadDbManager::getInstance()
{
    static auto instance = std::make_shared<chaynsThreadDbManager>();
    return instance;
}
void chaynsThreadDbManager::asyncUpsertThread(const ThreadRow&) {}
void chaynsThreadDbManager::asyncDetachThreadBySessionId(const std::string&) {}
void chaynsThreadDbManager::asyncUpdateThreadSessionId(const std::string&, const std::string&) {}
