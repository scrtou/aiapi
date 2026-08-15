#include <drogon/drogon_test.h>

#include <controllers/RetiredProviderTombstone.h>
#include <accountManager/accountManager.h>
#include <channelManager/channelManager.h>
#include <domain/port/IAccountStore.h>
#include <domain/port/IChannelStore.h>
#include <infrastructure/config/ConfigValidator.h>
#include <domain/policy/RetiredProviderPolicy.h>

#include <array>
#include <list>
#include <memory>
#include <string>

namespace {

Json::Value minimalConfig()
{
    Json::Value root(Json::objectValue);
    root["listeners"] = Json::arrayValue;
    Json::Value listener(Json::objectValue);
    listener["address"] = "127.0.0.1";
    listener["port"] = 55555;
    root["listeners"].append(listener);
    root["db_clients"] = Json::arrayValue;
    root["custom_config"] = Json::objectValue;
    return root;
}

bool containsError(const ConfigValidator::ValidationResult& result,
                   const std::string& needle)
{
    for (const auto& error : result.errors) {
        if (error.find(needle) != std::string::npos) return true;
    }
    return false;
}

class RetirementAccountStore final : public IAccountStore
{
  public:
    int waitingCreates = 0;
    bool addAccount(Accountinfo_st) override { return true; }
    bool updateAccount(Accountinfo_st) override { return true; }
    bool deleteAccount(std::string, std::string) override { return true; }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    std::list<Accountinfo_st> getAccountDBList() override { return {}; }
    int createWaitingAccount(std::string) override { ++waitingCreates; return 1; }
    bool activateAccount(int, Accountinfo_st) override { return true; }
    bool deleteWaitingAccount(int) override { return true; }
    int countAccountsByChannel(std::string, bool) override { return 0; }
    bool updateAccountStatusById(int, std::string) override { return true; }
    std::string getAccountStatusByUsername(std::string, std::string) override { return {}; }
};

class RetirementChannelStore final : public IChannelStore
{
  public:
    int writes = 0;
    bool addChannel(Channelinfo_st) override { ++writes; return true; }
    bool updateChannel(Channelinfo_st) override { ++writes; return true; }
    bool deleteChannel(int) override { return true; }
    bool getChannel(std::string, Channelinfo_st&) override { return false; }
    std::list<Channelinfo_st> getChannelList() override { return {}; }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    bool updateChannelStatus(std::string, bool) override { ++writes; return true; }
};

}  // namespace

DROGON_TEST(ProviderRetirement_TombstoneReturnsStable410ForEveryLegacyMethod)
{
    struct Case { drogon::HttpMethod method; const char* path; };
    const std::array<Case, 6> cases{{
        {drogon::Post, "/nexosapi/v1/chat/completions"},
        {drogon::Get, "/nexosapi/v1/models"},
        {drogon::Get, "/nexosapi/v1/account/quota"},
        {drogon::Post, "/nexosapi/v1/responses"},
        {drogon::Get, "/nexosapi/v1/responses/resp_1"},
        {drogon::Delete, "/nexosapi/v1/responses/resp_1"},
    }};

    for (const auto& item : cases) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(item.method);
        req->setPath(item.path);
        drogon::HttpResponsePtr captured;
        retired_provider::respondNexosTombstone(
            req, [&captured](const drogon::HttpResponsePtr& response) {
                captured = response;
            });
        REQUIRE(captured != nullptr);
        CHECK(static_cast<int>(captured->statusCode()) == 410);
        CHECK(captured->getHeader("X-AIAPI-Retirement-Id") ==
              retired_provider::kNexosRetirementId);
        auto json = captured->getJsonObject();
        REQUIRE(json != nullptr);
        CHECK((*json)["error"]["code"].asString() == "provider_retired");
        CHECK((*json)["retirement_id"].asString() ==
              retired_provider::kNexosRetirementId);
        REQUIRE((*json)["replacement_providers"].size() == 2);
        CHECK((*json)["replacement_providers"][0].asString() == "chaynsapi");
        CHECK((*json)["replacement_providers"][1].asString() == "retoolapi");
    }
}

DROGON_TEST(ProviderRetirement_TombstoneMetricContainsNoCredentials)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath("/nexosapi/v1/chat/completions");
    req->addHeader("X-Request-ID", "req-retired-1");
    req->addHeader("Authorization", "Bearer must-not-appear");
    req->setBody("{\"secret\":\"must-not-appear\"}");

    const auto metric = retired_provider::makeNexosTombstoneMetric(req);
    CHECK(metric.requestId == "req-retired-1");
    CHECK(metric.path == "/nexosapi/v1/chat/completions");
    CHECK(metric.method == "POST");
    const auto serialized = metric.detail.toStyledString();
    CHECK(serialized.find("must-not-appear") == std::string::npos);
    CHECK(metric.detail["retirement_id"].asString() ==
          retired_provider::kNexosRetirementId);
}

DROGON_TEST(ProviderRetirement_ConfigRejectsLegacyProviderKeysWithExactPaths)
{
    auto config = minimalConfig();
    auto& custom = config["custom_config"];
    custom["providers"]["openai"]["api_key"] = "ignored";
    custom["providers"]["nexos"]["base_url"] = "ignored";
    custom["outbound_limits"]["nexosapi"]["max_request_bytes"] = 1;
    custom["downstream_service_api_keys"] = Json::arrayValue;
    Json::Value service(Json::objectValue);
    service["name"] = "nexosapi";
    service["api_key"] = "ignored";
    custom["downstream_service_api_keys"].append(service);
    Json::Value account(Json::objectValue);
    account["apiname"] = "openai";
    custom["account"] = Json::arrayValue;
    custom["account"].append(account);
    custom["tool_bridge"]["format_by_channel"]["nexosapi"] = "json";
    custom["tool_bridge"]["strict_sentinel_enabled_channels"] = Json::arrayValue;
    custom["tool_bridge"]["strict_sentinel_enabled_channels"].append("openai");

    const auto result = ConfigValidator::validate(config);
    CHECK(!result.valid);
    CHECK(containsError(result, "custom_config.providers.openai"));
    CHECK(containsError(result, "custom_config.providers.nexos"));
    CHECK(containsError(result, "custom_config.outbound_limits.nexosapi"));
    CHECK(containsError(result, "custom_config.downstream_service_api_keys[0].name"));
    CHECK(containsError(result, "custom_config.account[0].apiname"));
    CHECK(containsError(result, "custom_config.tool_bridge.format_by_channel.nexosapi"));
    CHECK(containsError(result,
                        "custom_config.tool_bridge.strict_sentinel_enabled_channels[0]"));
}

DROGON_TEST(ProviderRetirement_ConfigKeepsLegalOpenAiBusinessFields)
{
    auto config = minimalConfig();
    config["custom_config"]["retool_workspace"]["openai_resource_uuid"] =
        "legal-retool-resource";
    const auto result = ConfigValidator::validate(config);
    CHECK(result.valid);
    CHECK(!containsError(result, "openai_resource_uuid"));
}

DROGON_TEST(ProviderRetirement_PolicyRejectsOnlyConcreteRetiredProviderKeys)
{
    CHECK(retired_provider::isRetiredProviderKey("nexosapi"));
    CHECK(retired_provider::isRetiredProviderKey("openai"));
    CHECK(!retired_provider::isRetiredProviderKey("chaynsapi"));
    CHECK(!retired_provider::isRetiredProviderKey("retoolapi"));
    CHECK(!retired_provider::isRetiredProviderKey("openai_resource_uuid"));
}

DROGON_TEST(ProviderRetirement_AccountManagerRejectsRetiredWritesBeforeStore)
{
    auto store = std::make_shared<RetirementAccountStore>();
    AccountManager manager;
    manager.setStore(store);
    manager.loadAccount();

    Accountinfo_st account;
    account.apiName = "nexosapi";
    account.userName = "retired@example.invalid";
    account.status = AccountStatus::ACTIVE;

    manager.addAccount(account.apiName, account.userName, "", "", 0, false, false,
                       0, "", "", "free", account.status);
    const auto accountList = manager.getAccountList();
    CHECK(accountList.find(account.apiName) == accountList.end());
    CHECK(!manager.addAccountbyPost(account));
    CHECK(!manager.updateAccount(account));
    CHECK(!manager.autoRegisterAccount(account.apiName));
    CHECK(store->waitingCreates == 0);
}

DROGON_TEST(ProviderRetirement_ChannelManagerRejectsRetiredWritesBeforeStore)
{
    auto store = std::make_shared<RetirementChannelStore>();
    ChannelManager manager;
    manager.setStore(store);

    Channelinfo_st channel;
    channel.channelName = "nexosapi";
    channel.channelType = "nexosapi";
    CHECK(!manager.addChannel(channel));
    CHECK(!manager.updateChannel(channel));
    CHECK(!manager.updateChannelStatus(channel.channelName, true));
    CHECK(store->writes == 0);
}
