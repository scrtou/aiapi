#include <drogon/drogon_test.h>

#include <infrastructure/account/AccountClock.h>
#include <domain/port/IAccountClock.h>
#include <domain/port/IAccountHttpTransport.h>
#include <application/account/accountManager.h>
#include <domain/port/IAccountStore.h>
#include <domain/port/IChannelStore.h>

#include <deque>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

class LifecycleAccountStore final : public IAccountStore
{
  public:
    std::list<Accountinfo_st> rows;
    int waitingId = 42;
    std::vector<std::string> statusLog;
    std::vector<int> deletedWaiting;
    std::vector<Accountinfo_st> activated;

    bool addAccount(Accountinfo_st row) override { rows.push_back(std::move(row)); return true; }
    bool updateAccount(Accountinfo_st) override { return true; }
    bool deleteAccount(std::string api, std::string user) override
    {
        for (auto it = rows.begin(); it != rows.end(); ++it)
            if (it->apiName == api && it->userName == user) { rows.erase(it); return true; }
        return false;
    }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    std::list<Accountinfo_st> getAccountDBList() override { return rows; }
    int createWaitingAccount(std::string api) override { createdWaitingApi = std::move(api); return waitingId; }
    bool activateAccount(int id, Accountinfo_st row) override
    {
        if (id != waitingId) return false;
        activated.push_back(std::move(row));
        return true;
    }
    bool deleteWaitingAccount(int id) override { deletedWaiting.push_back(id); return true; }
    int countAccountsByChannel(std::string, bool) override { return 0; }
    bool updateAccountStatusById(int id, std::string status) override
    {
        statusLog.push_back(std::to_string(id) + ":" + status);
        return true;
    }
    std::string getAccountStatusByUsername(std::string, std::string) override { return ""; }
    std::string createdWaitingApi;
};

class LifecycleChannelStore final : public IChannelStore
{
  public:
    std::list<Channelinfo_st> rows;
    bool addChannel(Channelinfo_st row) override { rows.push_back(std::move(row)); return true; }
    bool updateChannel(Channelinfo_st) override { return true; }
    bool deleteChannel(int) override { return true; }
    bool getChannel(std::string name, Channelinfo_st& out) override
    {
        for (const auto& row : rows) if (row.channelName == name) { out = row; return true; }
        return false;
    }
    std::list<Channelinfo_st> getChannelList() override { return rows; }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    bool updateChannelStatus(std::string, bool) override { return true; }
};

struct ScriptedResponse
{
    int status = 200;
    Json::Value body{Json::objectValue};
};

class LifecycleHttpTransport final : public account::IAccountHttpTransport
{
  public:
    void push(int status, Json::Value body = Json::Value(Json::objectValue))
    {
        responses.push_back({status, std::move(body)});
    }
    account::HttpResult send(const std::string& baseUrl,
                             const account::HttpRequest& request,
                             double timeoutSeconds) override
    {
        baseUrls.push_back(baseUrl);
        paths.push_back(request.path);
        methods.push_back(request.method);
        authorizationPresent.push_back(
            request.headers.find("Authorization") != request.headers.end());
        if (timeoutSeconds <= 0 || responses.empty())
            return {account::HttpResultCode::BadResponse, nullptr};
        const auto scripted = responses.front();
        responses.pop_front();
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        auto response = std::make_shared<account::HttpResponse>();
        response->statusCode = scripted.status;
        response->body = Json::writeString(writer, scripted.body);
        response->headers["content-type"] = "application/json";
        return {account::HttpResultCode::Ok, std::move(response)};
    }
    std::deque<ScriptedResponse> responses;
    std::vector<std::string> baseUrls;
    std::vector<std::string> paths;
    std::vector<account::HttpMethod> methods;
    std::vector<bool> authorizationPresent;
};

class LifecycleClock final : public account::IAccountClock
{
  public:
    void sleepFor(std::chrono::milliseconds duration) override
    {
        ++calls;
        sleeps.push_back(duration);
    }
    int calls = 0;
    std::vector<std::chrono::milliseconds> sleeps;
};

Accountinfo_st accountRow(const std::string& api, const std::string& user)
{
    Accountinfo_st row;
    row.apiName = api;
    row.userName = user;
    row.passwd = "synthetic-password";
    row.authToken = "synthetic-token";
    row.useCount = 0;
    row.tokenStatus = true;
    row.accountStatus = true;
    row.personId = "synthetic-person";
    row.accountType = "free";
    row.status = AccountStatus::ACTIVE;
    return row;
}

std::shared_ptr<LifecycleChannelStore> installActiveChaynsChannel()
{
    auto channels = std::make_shared<LifecycleChannelStore>();
    Channelinfo_st channel;
    channel.channelName = "chaynsapi";
    channel.channelStatus = true;
    channels->rows.push_back(channel);
    return channels;
}

Json::Value registrationCreated()
{
    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"]["task_id"] = "synthetic-task";
    return body;
}

Json::Value registrationSucceeded()
{
    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"]["task"]["status"] = "succeeded";
    body["data"]["task"]["state"] = "completed";
    auto& result = body["data"]["result"];
    result["registration"]["account"]["email"] = "synthetic-user.invalid";
    result["registration"]["account"]["password"] = "synthetic-password";
    result["login"]["session"]["access_token"] = "synthetic-access-token";
    result["login"]["site_result"]["userid"] = 1001;
    result["login"]["site_result"]["personid"] = "synthetic-person";
    result["login"]["site_result"]["has_pro_access"] = true;
    return body;
}

}  // namespace

DROGON_TEST(AccountLifecycle_InMemoryAddUpdateDeleteCharacterization)
{
    auto store = std::make_shared<LifecycleAccountStore>();
    AccountManager manager;
    manager.setStore(store);
    manager.loadAccount();

    auto row = accountRow("fixture-api", "fixture-user");
    CHECK(manager.addAccountbyPost(row));
    CHECK(!manager.addAccountbyPost(row));
    std::shared_ptr<Accountinfo_st> loaded;
    manager.getAccountByUserName(row.apiName, row.userName, loaded);
    REQUIRE(loaded != nullptr);
    // Legacy lookup-by-name is not a pure read: it increments useCount.
    CHECK(loaded->useCount == 1);

    row.useCount = 7;
    row.tokenStatus = false;
    CHECK(manager.updateAccount(row));
    manager.getAccountByUserName(row.apiName, row.userName, loaded);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->useCount == 7);
    CHECK(!loaded->tokenStatus);
    CHECK(manager.deleteAccountbyPost(row.apiName, row.userName));
    CHECK(!manager.deleteAccountbyPost(row.apiName, row.userName));
    manager.getAccountByUserName(row.apiName, row.userName, loaded);
    CHECK(loaded == nullptr);
    // These legacy methods mutate only the in-memory index/pool.
    CHECK(store->rows.empty());
}

DROGON_TEST(AccountLifecycle_CheckTokenUsesFakeHttpAndInvalidatesPool)
{
    auto store = std::make_shared<LifecycleAccountStore>();
    store->rows.push_back(accountRow("chaynsapi", "token-user"));
    auto transport = std::make_shared<LifecycleHttpTransport>();
    transport->push(401);
    AccountManager manager;
    manager.setStore(store);
    manager.setHttpTransport(transport);
    manager.setClock(std::make_shared<LifecycleClock>());
    manager.loadAccount();

    manager.checkToken();
    const auto accounts = manager.getAccountList();
    REQUIRE(accounts.at("chaynsapi").at("token-user") != nullptr);
    CHECK(!accounts.at("chaynsapi").at("token-user")->tokenStatus);
    std::shared_ptr<Accountinfo_st> eligible;
    CHECK(!manager.getEligibleAccount(
        "chaynsapi", eligible, AccountRequirement::AnyValid));
    CHECK(eligible == nullptr);
    REQUIRE(transport->paths.size() == 1);
    CHECK(transport->paths[0] == "/AccountService/v1.0/Chayns/User");
    CHECK(transport->methods[0] == account::HttpMethod::Get);
    CHECK(transport->authorizationPresent[0]);
}

DROGON_TEST(AccountLifecycle_AutoRegisterHttpFailureRollsBackWaitingRow)
{
    auto channels = installActiveChaynsChannel();
    auto store = std::make_shared<LifecycleAccountStore>();
    auto transport = std::make_shared<LifecycleHttpTransport>();
    transport->push(503);
    auto clock = std::make_shared<LifecycleClock>();
    AccountManager manager;
    manager.setStore(store);
    manager.setChannelStore(channels);
    manager.setHttpTransport(transport);
    manager.setClock(clock);

    CHECK(!manager.autoRegisterAccount("chaynsapi"));
    CHECK(store->createdWaitingApi == "chaynsapi");
    REQUIRE(store->statusLog.size() == 2);
    CHECK(store->statusLog[0] == "42:registering");
    CHECK(store->statusLog[1] == "42:waiting");
    REQUIRE(store->deletedWaiting.size() == 1);
    CHECK(store->deletedWaiting[0] == 42);
    CHECK(!manager.isAccountRegistering(42));
    CHECK(clock->calls == 0);
}

DROGON_TEST(AccountLifecycle_AutoRegisterSuccessActivatesAndLoadsAccount)
{
    auto channels = installActiveChaynsChannel();
    auto store = std::make_shared<LifecycleAccountStore>();
    auto transport = std::make_shared<LifecycleHttpTransport>();
    transport->push(200, registrationCreated());
    transport->push(200, registrationSucceeded());
    auto clock = std::make_shared<LifecycleClock>();
    AccountManager manager;
    manager.setStore(store);
    manager.setChannelStore(channels);
    manager.setHttpTransport(transport);
    manager.setClock(clock);
    manager.loadAccount();

    CHECK(manager.autoRegisterAccount("chaynsapi"));
    REQUIRE(store->activated.size() == 1);
    CHECK(store->activated[0].userName == "synthetic-user.invalid");
    CHECK(store->activated[0].accountType == "pro");
    std::shared_ptr<Accountinfo_st> loaded;
    manager.getAccountByUserName("chaynsapi", "synthetic-user.invalid", loaded);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->personId == "synthetic-person");
    CHECK(!manager.isAccountRegistering(42));
    CHECK(transport->responses.empty());
    REQUIRE(transport->paths.size() == 2);
    CHECK(transport->paths[0] == "/api/v1/workflows/register-and-login");
    CHECK(transport->paths[1] == "/api/v1/workflows/synthetic-task");
    CHECK(clock->calls == 0);
}
