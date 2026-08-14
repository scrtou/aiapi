#include <drogon/drogon_test.h>

#include <application/account/AccountAdminUseCase.h>

// ARCH_TESTS: application/account/AccountAdminUseCase.h
// ARCH_TESTS: domain/port/IAccountAdminUseCase.h

namespace {

class AdminCatalog final : public IAccountCatalog
{
  public:
    AccountMap values;
    AccountMap listAccounts() override { return values; }
    void checkChannelAccountCount(std::string) override {}
};

class AdminCommands final : public IAccountAdminCommands
{
  public:
    int tokenChecks = 0;
    int typeRefreshes = 0;
    int autoRegisterCalls = 0;
    AccountAutomationSettings settings;

    bool addAccountbyPost(Accountinfo_st) override { return true; }
    bool updateAccount(Accountinfo_st) override { return true; }
    bool deleteAccountbyPost(std::string, std::string) override { return true; }
    bool isAccountRegisteringByUsername(const std::string&) override { return false; }
    bool deleteUpstreamAccount(const Accountinfo_st&) override { return true; }
    void loadAccount() override {}
    void checkUpdateAccountToken() override {}
    void updateAccountType(std::shared_ptr<Accountinfo_st>) override {}
    void checkChannelAccountCounts() override {}
    void checkToken() override { ++tokenChecks; }
    void updateAllAccountTypes() override { ++typeRefreshes; }
    bool autoRegisterAccount(std::string) override { ++autoRegisterCalls; return true; }
    AccountAutomationSettings getAccountAutomationSettings() const override { return settings; }
    bool updateAccountAutomationSettings(
        const AccountAutomationSettings& value, bool, std::string*) override
    { settings = value; return true; }
};

class AdminStore final : public IAccountStore
{
  public:
    std::list<Accountinfo_st> rows;
    bool addAccount(Accountinfo_st value) override { rows.push_back(value); return true; }
    bool updateAccount(Accountinfo_st) override { return true; }
    bool deleteAccount(std::string, std::string) override { return true; }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    std::list<Accountinfo_st> getAccountDBList() override { return rows; }
    int createWaitingAccount(std::string) override { return 0; }
    bool activateAccount(int, Accountinfo_st) override { return true; }
    bool deleteWaitingAccount(int) override { return true; }
    int countAccountsByChannel(std::string, bool) override { return 0; }
    bool updateAccountStatusById(int, std::string) override { return true; }
    std::string getAccountStatusByUsername(std::string, std::string) override { return {}; }
};

class AdminBackups final : public IAccountBackupStore
{
  public:
    std::list<Accountinfo_st> rows;
    std::list<Accountinfo_st> listBackupAccounts() override { return rows; }
};

class AdminChannels final : public IChannelCatalog
{
  public:
    std::list<Channelinfo_st> rows;
    std::list<Channelinfo_st> listChannels() const override { return rows; }
    bool addChannel(Channelinfo_st) override { return false; }
    bool updateChannel(Channelinfo_st) override { return false; }
    bool deleteChannel(int) override { return false; }
    bool updateChannelStatus(std::string, bool) override { return false; }
    std::optional<bool> supportsToolCalls(const std::string&) const override { return std::nullopt; }
};

class AdminExecutor final : public IBackgroundExecutor
{
  public:
    TaskSubmitResult result = TaskSubmitResult::Accepted;
    TaskSubmitResult submit(const std::string&, std::function<void()> task) override
    { if (result == TaskSubmitResult::Accepted) task(); return result; }
};

}  // namespace

DROGON_TEST(AccountAdminUseCaseRefreshRunsThroughInjectedExecutor)
{
    AdminCatalog catalog; AdminCommands commands; AdminStore store;
    AdminBackups backups; AdminChannels channels; AdminExecutor executor;
    AccountAdminUseCase useCase(&catalog, &commands, &store, &backups, &channels, &executor);
    CHECK(useCase.refreshAccounts() == TaskSubmitResult::Accepted);
    CHECK(commands.tokenChecks == 1);
    CHECK(commands.typeRefreshes == 1);
}

DROGON_TEST(AccountAdminUseCasePropagatesExecutorRejection)
{
    AdminCatalog catalog; AdminCommands commands; AdminStore store;
    AdminBackups backups; AdminChannels channels; AdminExecutor executor;
    executor.result = TaskSubmitResult::QueueFull;
    AccountAdminUseCase useCase(&catalog, &commands, &store, &backups, &channels, &executor);
    CHECK(useCase.refreshAccounts() == TaskSubmitResult::QueueFull);
    CHECK(commands.tokenChecks == 0);
}

DROGON_TEST(AccountAdminUseCaseQueriesInjectedChannelCatalog)
{
    AdminCatalog catalog; AdminCommands commands; AdminStore store;
    AdminBackups backups; AdminChannels channels; AdminExecutor executor;
    Channelinfo_st channel; channel.channelName = "chaynsapi"; channel.channelStatus = false;
    channels.rows.push_back(channel);
    AccountAdminUseCase useCase(&catalog, &commands, &store, &backups, &channels, &executor);
    REQUIRE(useCase.channelEnabled("chaynsapi").has_value());
    CHECK(!*useCase.channelEnabled("chaynsapi"));
    CHECK(!useCase.channelEnabled("missing").has_value());
}
