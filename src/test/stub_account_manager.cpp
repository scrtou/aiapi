// 测试专用桩实现：
// RequestAdapters.cpp 只用到 AccountManager 单例的 getAccountAutomationSettings()。
// 真实的 accountManager.cpp 会连带引入 AccountDbManager / ChannelManager /
// ConfigDbManager / RetoolWorkspaceManager 等大量数据库依赖，不适合链接进单元测试。
// 这里提供最小桩实现，保证链接通过且返回默认配置。
#include <accountManager/accountManager.h>
#include <dbManager/account/accountDbManager.h>

AccountManager::AccountManager() = default;
AccountManager::~AccountManager() = default;

AccountAutomationSettings AccountManager::getAccountAutomationSettings() const
{
    std::lock_guard<std::mutex> lock(accountAutomationSettingsMutex_);
    return accountAutomationSettings_;
}

// 成员函数指针表（updateTokenMap / checkTokenMap）是类内默认初始化器的一部分，
// 构造函数会 odr-use 这四个成员函数，因此必须提供定义。
// 单元测试不涉及真实 token 刷新，这里给出空实现。
void AccountManager::updateChaynsToken(std::shared_ptr<Accountinfo_st>) {}
void AccountManager::updateNexosToken(std::shared_ptr<Accountinfo_st>) {}
bool AccountManager::checkChaynsToken(std::string) { return true; }
bool AccountManager::checkNexosToken(std::string) { return true; }
