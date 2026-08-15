#include <accountManager/accountManager.h>

#include <accountManager/AccountWorkflowSupport.h>
#include <platform/Log.h>

#include <chrono>
#include <optional>
#include <utility>

namespace {

bool parseBoolConfigValue(const std::string& value, bool& parsed)
{
    if (value == "true" || value == "1") {
        parsed = true;
        return true;
    }
    if (value == "false" || value == "0") {
        parsed = false;
        return true;
    }
    return false;
}

bool parsePositiveIntConfigValue(const std::string& value, int& parsed)
{
    try {
        parsed = std::stoi(value);
        return parsed > 0;
    } catch (...) {
        return false;
    }
}

class NullAccountStore final : public IAccountStore
{
  public:
    bool addAccount(Accountinfo_st) override { return false; }
    bool updateAccount(Accountinfo_st) override { return false; }
    bool deleteAccount(std::string, std::string) override { return false; }
    bool isTableExist() override { return false; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    std::list<Accountinfo_st> getAccountDBList() override { return {}; }
    int createWaitingAccount(std::string) override { return -1; }
    bool activateAccount(int, Accountinfo_st) override { return false; }
    bool deleteWaitingAccount(int) override { return false; }
    int countAccountsByChannel(std::string, bool) override { return 0; }
    bool updateAccountStatusById(int, std::string) override { return false; }
    std::string getAccountStatusByUsername(std::string, std::string) override { return {}; }
};

class NullChannelStoreForAccount final : public IChannelStore
{
  public:
    bool addChannel(Channelinfo_st) override { return false; }
    bool updateChannel(Channelinfo_st) override { return false; }
    bool deleteChannel(int) override { return false; }
    bool getChannel(std::string, Channelinfo_st&) override { return false; }
    std::list<Channelinfo_st> getChannelList() override { return {}; }
    bool isTableExist() override { return false; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    bool updateChannelStatus(std::string, bool) override { return false; }
};

class NullConfigStoreForAccount final : public IKeyValueConfigStore
{
  public:
    bool ensureTable(std::string* error) override
    {
        if (error) *error = "AccountManager: config store 未注入";
        return false;
    }
    std::optional<std::string> getValue(const std::string&, std::string*) override
    {
        return std::nullopt;
    }
    bool setValues(const std::map<std::string, std::string>&, std::string* error) override
    {
        if (error) *error = "AccountManager: config store 未注入";
        return false;
    }
};

// AccountManager is an application service.  Concrete network/time adapters
// belong to infrastructure and are injected by AppWiring before init(); these
// fallbacks make an omitted test injection safe without recreating a reverse
// application -> infrastructure target edge.
class UnconfiguredAccountHttpTransport final : public account::IAccountHttpTransport
{
  public:
    account::HttpResult send(const std::string&, const account::HttpRequest&, double) override
    {
        return {account::HttpResultCode::BadResponse, nullptr};
    }
};

class UnconfiguredAccountClock final : public account::IAccountClock
{
  public:
    void sleepFor(std::chrono::milliseconds) override {}
};

}  // namespace

AccountManager::AccountManager()
    : httpTransport_(std::make_shared<UnconfiguredAccountHttpTransport>()),
      clock_(std::make_shared<UnconfiguredAccountClock>()),
      registrationStateMachine_(std::make_unique<account::AccountRegistrationStateMachine>())
{
    static NullConfigStoreForAccount nullConfig;
    configStore_ = std::shared_ptr<IKeyValueConfigStore>(&nullConfig, [](IKeyValueConfigStore*) {});
}

AccountManager::~AccountManager()
{
    stopBackgroundThreads();
}

void AccountManager::setStore(std::shared_ptr<IAccountStore> store)
{
    accountDbManager = std::move(store);
    registrationStateMachine_->setStore(accountDbManager.get());
}

void AccountManager::setChannelStore(std::shared_ptr<IChannelStore> store)
{
    channelStore = std::move(store);
}

void AccountManager::setHttpTransport(std::shared_ptr<account::IAccountHttpTransport> transport)
{
    if (transport) httpTransport_ = std::move(transport);
}

void AccountManager::setClock(std::shared_ptr<account::IAccountClock> clock)
{
    if (clock) clock_ = std::move(clock);
}

void AccountManager::setRetoolProvisionClock(
    std::shared_ptr<retoolProvision::IRetoolProvisionClock> clock)
{
    if (clock) retoolProvisionClock_ = std::move(clock);
}

void AccountManager::setConfigStore(std::shared_ptr<IKeyValueConfigStore> store)
{
    if (store) configStore_ = std::move(store);
}

void AccountManager::setRuntimeConfig(Json::Value config)
{
    runtimeConfig_ = config.isObject() ? std::move(config) : Json::Value(Json::objectValue);
}

void AccountManager::setRetoolWorkspaceServices(
    workspace::IRetoolWorkspaceUseCase* useCase,
    workspace::IRetoolWorkspaceProvisioner* provisioner)
{
    workspaceUseCase_ = useCase;
    workspaceProvisioner_ = provisioner;
}

account::HttpResult AccountManager::sendHttpRequest(
    const std::string& baseUrl,
    const account::HttpRequest& request,
    double timeoutSeconds) const
{
    return httpTransport_ ? httpTransport_->send(baseUrl, request, timeoutSeconds)
                          : account::HttpResult{account::HttpResultCode::BadResponse, nullptr};
}

IAccountStore* AccountManager::requireStore()
{
    if (accountDbManager) return accountDbManager.get();
    static NullAccountStore nullStore;
    LOG_ERROR << "[账号管理] store 未注入，回退 NullAccountStore";
    accountDbManager = std::shared_ptr<IAccountStore>(&nullStore, [](IAccountStore*) {});
    registrationStateMachine_->setStore(accountDbManager.get());
    return accountDbManager.get();
}

IChannelStore* AccountManager::requireChannelStore()
{
    if (channelStore) return channelStore.get();
    static NullChannelStoreForAccount nullStore;
    LOG_ERROR << "[账号管理] channelStore 未注入，回退 NullChannelStoreForAccount";
    channelStore = std::shared_ptr<IChannelStore>(&nullStore, [](IChannelStore*) {});
    return channelStore.get();
}

void AccountManager::loadAccountAutomationSettings()
{
    const auto defaults = account::workflow::loadAutomationSettingsFromCustomConfig(
        runtimeConfig_);
    auto settings = defaults;
    auto* configStore = configStore_.get();
    std::string error;
    bool loadedFromDb = false;

    if (!configStore->ensureTable(&error)) {
        LOG_WARN << "[账户管理] " << error << "，账号自动化策略回退为配置文件默认值";
    } else {
        bool hasStored = false;
        bool seedDefaults = false;
        const auto loadBool = [&](const char* key, bool& value) {
            if (const auto stored = configStore->getValue(key, &error)) {
                if (parseBoolConfigValue(*stored, value)) {
                    hasStored = true;
                } else {
                    seedDefaults = true;
                }
            }
        };
        loadBool("account_automation.auto_delete_enabled", settings.autoDeleteEnabled);
        loadBool("account_automation.auto_register_enabled", settings.autoRegisterEnabled);
        loadBool("tool_bridge.namespace_enabled", settings.namespaceToolBridgeEnabled);
        if (const auto stored = configStore->getValue("account_automation.delete_after_days", &error)) {
            if (parsePositiveIntConfigValue(*stored, settings.deleteAfterDays)) {
                hasStored = true;
            } else {
                seedDefaults = true;
                settings.deleteAfterDays = defaults.deleteAfterDays;
            }
        }
        if (!hasStored) {
            seedDefaults = true;
            settings = defaults;
        } else {
            loadedFromDb = true;
        }
        if (seedDefaults && !account::workflow::saveAutomationSettings(*configStore, settings, &error)) {
            LOG_WARN << "[账户管理] 初始化账号自动化配置到数据库失败: " << error;
        }
    }

    {
        std::lock_guard<std::mutex> lock(accountAutomationSettingsMutex_);
        accountAutomationSettings_ = settings;
    }
    LOG_INFO << "[账户管理] 自动化策略已加载(" << (loadedFromDb ? "db" : "config") << ")";
}

AccountAutomationSettings AccountManager::getAccountAutomationSettings() const
{
    std::lock_guard<std::mutex> lock(accountAutomationSettingsMutex_);
    return accountAutomationSettings_;
}

bool AccountManager::updateAccountAutomationSettings(const AccountAutomationSettings& settings,
                                                     bool persistToConfig,
                                                     std::string* errorMessage)
{
    if (settings.deleteAfterDays <= 0) {
        if (errorMessage) *errorMessage = "deleteAfterDays 必须为正整数";
        return false;
    }
    if (persistToConfig) {
        if (!configStore_->ensureTable(errorMessage) ||
            !account::workflow::saveAutomationSettings(*configStore_, settings, errorMessage)) {
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(accountAutomationSettingsMutex_);
        accountAutomationSettings_ = settings;
    }
    return true;
}

void AccountManager::init()
{
    LOG_INFO << "[账户管理] 初始化开始";
    auto* store = requireStore();
    if (!store->isTableExist()) {
        store->createTable();
    } else {
        store->checkAndUpgradeTable();
    }
    loadAccountAutomationSettings();
    loadAccount();

    const auto& config = runtimeConfig_;
    const bool startWorkers = !config.isMember("account_background_threads_enabled") ||
        config["account_background_threads_enabled"].asBool();
    if (startWorkers) {
        checkUpdateTokenthread();
        waitUpdateAccountTokenThread();
        checkAccountTypeThread();
    }
    LOG_INFO << "[账户管理] 登录服务URL: "
             << account::workflow::loginServiceUrl(runtimeConfig_, "chaynsapi");
    LOG_INFO << "[账户管理] 注册服务URL: "
             << account::workflow::registrationServiceUrl(runtimeConfig_, "chaynsapi");
}
