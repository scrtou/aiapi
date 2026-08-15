#include <drogon/drogon_test.h>

#include <accountManager/AccountRegistrationStateMachine.h>
#include <accountManager/AccountSelectionPolicy.h>
#include <accountManager/AccountWorkflowSupport.h>
#include <domain/port/IAccountStore.h>

#include <list>
#include <memory>
#include <string>
#include <vector>

// ARCH_TESTS: accountManager/AccountRegistrationStateMachine.h
// ARCH_TESTS: accountManager/AccountSelectionPolicy.h
// ARCH_TESTS: accountManager/AccountWorkflowSupport.h

namespace {

class TransitionStore final : public IAccountStore
{
  public:
    bool addAccount(Accountinfo_st) override { return true; }
    bool updateAccount(Accountinfo_st) override { return true; }
    bool deleteAccount(std::string, std::string) override { return true; }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    std::list<Accountinfo_st> getAccountDBList() override { return {}; }

    int createWaitingAccount(std::string) override { return waitingId; }
    bool activateAccount(int id, Accountinfo_st row) override
    {
        activated = id == waitingId;
        activatedRow = std::move(row);
        return activated;
    }
    bool deleteWaitingAccount(int id) override
    {
        operations.push_back("delete:" + std::to_string(id));
        return id == waitingId;
    }
    int countAccountsByChannel(std::string, bool) override { return 0; }
    bool updateAccountStatusById(int id, std::string status) override
    {
        operations.push_back("status:" + std::to_string(id) + ":" + status);
        return updateTransitionSucceeds;
    }
    std::string getAccountStatusByUsername(std::string, std::string) override
    {
        return usernameStatus;
    }

    int waitingId = 73;
    bool updateTransitionSucceeds = true;
    bool activated = false;
    Accountinfo_st activatedRow;
    std::string usernameStatus;
    std::vector<std::string> operations;
};

std::shared_ptr<Accountinfo_st> candidate(const std::string& type = "free")
{
    auto account = std::make_shared<Accountinfo_st>();
    account->apiName = "chaynsapi";
    account->userName = "candidate.invalid";
    account->authToken = "synthetic";
    account->tokenStatus = true;
    account->accountStatus = true;
    account->status = AccountStatus::ACTIVE;
    account->accountType = type;
    return account;
}

}  // namespace

DROGON_TEST(AccountWorkflow_StateMachineRollbackRestoresWaitingThenDeletes)
{
    TransitionStore store;
    account::AccountRegistrationStateMachine state(&store);

    const int waitingId = state.begin("chaynsapi");
    REQUIRE(waitingId == 73);
    CHECK(state.isRegistering(waitingId));
    REQUIRE(store.operations.size() == 1);
    CHECK(store.operations[0] == "status:73:registering");

    state.rollback(waitingId);
    REQUIRE(store.operations.size() == 3);
    CHECK(store.operations[1] == "status:73:waiting");
    CHECK(store.operations[2] == "delete:73");
    state.finish(waitingId);
    CHECK(!state.isRegistering(waitingId));
}

DROGON_TEST(AccountWorkflow_StateMachineTransitionFailureRollsBackReservation)
{
    TransitionStore store;
    store.updateTransitionSucceeds = false;
    account::AccountRegistrationStateMachine state(&store);

    CHECK(state.begin("chaynsapi") == -1);
    REQUIRE(store.operations.size() == 3);
    CHECK(store.operations[0] == "status:73:registering");
    CHECK(store.operations[1] == "status:73:waiting");
    CHECK(store.operations[2] == "delete:73");
    CHECK(!state.isRegistering(73));
}

DROGON_TEST(AccountWorkflow_SelectorAppliesBindingRequirementAndRotationFilter)
{
    auto free = candidate("free");
    CHECK(account::selection::isPoolMember(free));
    CHECK(account::selection::matchesRequirement(
        free, "chaynsapi", AccountRequirement::AnyValid, {}));
    CHECK(account::selection::matchesRequirement(
        free, "chaynsapi", AccountRequirement::FreeOnly, {}));
    CHECK(!account::selection::matchesRequirement(
        free, "chaynsapi", AccountRequirement::ProOnly, {}));

    auto pro = candidate("pro");
    CHECK(!account::selection::matchesRequirement(
        pro, "chaynsapi", AccountRequirement::AnyValid, {}));
    pro->workspaceUacId = 91;
    CHECK(account::selection::matchesRequirement(
        pro, "chaynsapi", AccountRequirement::ProOnly, {}));
    CHECK(!account::selection::matchesRequirement(
        pro, "chaynsapi", AccountRequirement::AnyValid, {pro->userName}));
}

DROGON_TEST(AccountWorkflow_SupportParsesWorkflowEndpointsAndEnvelopes)
{
    std::string baseUrl;
    std::string path;
    CHECK(account::workflow::splitUrl(
        "https://workflow.invalid/api/v1/register", baseUrl, path));
    CHECK(baseUrl == "https://workflow.invalid");
    CHECK(path == "/api/v1/register");
    CHECK(!account::workflow::splitUrl("workflow.invalid/no-scheme", baseUrl, path));

    Json::Value success;
    success["success"] = true;
    success["data"] = Json::Value(Json::objectValue);
    CHECK(account::workflow::isSuccessEnvelope(success));
    success["success"] = false;
    CHECK(!account::workflow::isSuccessEnvelope(success));
}

DROGON_TEST(AccountWorkflow_RuntimeConfigOverridesLifecycleEndpointsAndCredential)
{
    Json::Value config(Json::objectValue);
    config["login_service_urls"] = Json::arrayValue;
    config["login_service_urls"].append(Json::Value(Json::objectValue));
    config["login_service_urls"][0]["name"] = "chaynsapi";
    config["login_service_urls"][0]["url"] = "https://login.fixture.invalid/v2/login";
    config["regist_service_urls"] = Json::arrayValue;
    config["regist_service_urls"].append(Json::Value(Json::objectValue));
    config["regist_service_urls"][0]["name"] = "chaynsapi";
    config["regist_service_urls"][0]["url"] = "https://register.fixture.invalid/v2/register";
    config["downstream_service_api_keys"] = Json::arrayValue;
    config["downstream_service_api_keys"].append(Json::Value(Json::objectValue));
    config["downstream_service_api_keys"][0]["name"] = "chaynsapi";
    config["downstream_service_api_keys"][0]["api_key"] = "fixture-runtime-key";

    CHECK(account::workflow::loginServiceUrl(config, "chaynsapi") ==
          "https://login.fixture.invalid/v2/login");
    CHECK(account::workflow::registrationServiceUrl(config, "chaynsapi") ==
          "https://register.fixture.invalid/v2/register");
    CHECK(account::workflow::downstreamBearerApiKey(config, "chaynsapi") ==
          "fixture-runtime-key");
}
