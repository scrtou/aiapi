#include <application/account/AccountAdminUseCase.h>

AccountAdminUseCase::AccountAdminUseCase(
    IAccountCatalog* catalog, IAccountAdminCommands* commands,
    IAccountStore* store, IAccountBackupStore* backups,
    IChannelCatalog* channels, IBackgroundExecutor* executor)
    : catalog_(catalog), commands_(commands), store_(store), backups_(backups),
      channels_(channels), executor_(executor) {}

IAccountAdminUseCase::AccountMap AccountAdminUseCase::listAccounts()
{
    return catalog_ ? catalog_->listAccounts() : AccountMap{};
}

std::list<Accountinfo_st> AccountAdminUseCase::listBackupAccounts()
{
    return backups_ ? backups_->listBackupAccounts() : std::list<Accountinfo_st>{};
}

std::list<Accountinfo_st> AccountAdminUseCase::listStoredAccounts()
{
    return store_ ? store_->getAccountDBList() : std::list<Accountinfo_st>{};
}

std::optional<bool> AccountAdminUseCase::channelEnabled(const std::string& apiName) const
{
    if (!channels_) return std::nullopt;
    for (const auto& channel : channels_->listChannels())
        if (channel.channelName == apiName) return channel.channelStatus;
    return std::nullopt;
}

bool AccountAdminUseCase::stageAdd(Accountinfo_st account)
{
    return commands_ && commands_->addAccountbyPost(std::move(account));
}

bool AccountAdminUseCase::stageUpdate(Accountinfo_st account)
{
    return commands_ && commands_->updateAccount(std::move(account));
}

bool AccountAdminUseCase::isRegistering(const std::string& userName)
{
    return commands_ && commands_->isAccountRegisteringByUsername(userName);
}

bool AccountAdminUseCase::stageDelete(
    const std::string& apiName, const std::string& userName)
{
    return commands_ && commands_->deleteAccountbyPost(apiName, userName);
}

TaskSubmitResult AccountAdminUseCase::persistAdds(std::list<Accountinfo_st> accounts)
{
    if (!executor_ || !commands_ || !catalog_ || !store_) return TaskSubmitResult::Stopped;
    auto* commands = commands_; auto* catalog = catalog_; auto* store = store_;
    return executor_->submit("accountAdd", [accounts = std::move(accounts), commands, catalog, store]() {
        for (const auto& account : accounts) store->addAccount(account);
        commands->checkUpdateAccountToken();
        for (const auto& account : accounts) {
            auto map = catalog->listAccounts();
            auto api = map.find(account.apiName);
            if (api != map.end()) {
                auto user = api->second.find(account.userName);
                if (user != api->second.end()) commands->updateAccountType(user->second);
            }
        }
    });
}

TaskSubmitResult AccountAdminUseCase::persistUpdates(std::list<Accountinfo_st> accounts)
{
    if (!executor_ || !commands_ || !catalog_ || !store_) return TaskSubmitResult::Stopped;
    auto* commands = commands_; auto* catalog = catalog_; auto* store = store_;
    return executor_->submit("accountUpdate", [accounts = std::move(accounts), commands, catalog, store]() {
        for (const auto& account : accounts) store->updateAccount(account);
        for (const auto& account : accounts) {
            auto map = catalog->listAccounts();
            auto api = map.find(account.apiName);
            if (api != map.end()) {
                auto user = api->second.find(account.userName);
                if (user != api->second.end()) commands->updateAccountType(user->second);
            }
        }
    });
}

TaskSubmitResult AccountAdminUseCase::persistDeletes(std::list<Accountinfo_st> accounts)
{
    if (!executor_ || !commands_ || !store_) return TaskSubmitResult::Stopped;
    auto* commands = commands_; auto* store = store_;
    return executor_->submit("accountDelete", [accounts = std::move(accounts), commands, store]() {
        for (const auto& account : accounts) {
            commands->deleteUpstreamAccount(account);
            store->deleteAccount(account.apiName, account.userName);
        }
        commands->loadAccount();
        commands->checkChannelAccountCounts();
    });
}

TaskSubmitResult AccountAdminUseCase::refreshAccounts()
{
    if (!executor_ || !commands_) return TaskSubmitResult::Stopped;
    auto* commands = commands_;
    return executor_->submit("accountRefresh", [commands]() {
        commands->checkToken();
        commands->updateAllAccountTypes();
    });
}

TaskSubmitResult AccountAdminUseCase::autoRegister(std::string apiName, int count)
{
    if (!executor_ || !commands_) return TaskSubmitResult::Stopped;
    auto* commands = commands_;
    return executor_->submit("accountAutoRegister", [apiName = std::move(apiName), count, commands]() {
        commands->autoRegisterAccounts(apiName, count);
    });
}

AccountAutomationSettings AccountAdminUseCase::automationSettings() const
{
    return commands_ ? commands_->getAccountAutomationSettings()
                     : AccountAutomationSettings{};
}

bool AccountAdminUseCase::updateAutomationSettings(
    const AccountAutomationSettings& settings, std::string* errorMessage)
{
    return commands_ && commands_->updateAccountAutomationSettings(
        settings, true, errorMessage);
}
