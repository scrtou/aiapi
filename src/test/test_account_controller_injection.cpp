#include <drogon/drogon_test.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <transport/controllers/AccountController.h>

// ARCH_TESTS: domain/port/IAccountAdminCommands.h
// ARCH_TESTS: domain/port/IAccountBackupStore.h

namespace {

class AccountAdminUseCaseFake final : public IAccountAdminUseCase
{
  public:
    AccountMap values;
    int listCalls = 0;

    AccountMap listAccounts() override { ++listCalls; return values; }
    std::list<Accountinfo_st> listBackupAccounts() override { return {}; }
    std::list<Accountinfo_st> listStoredAccounts() override { return {}; }
    std::optional<bool> channelEnabled(const std::string&) const override { return true; }
    bool stageAdd(Accountinfo_st) override { return false; }
    bool stageUpdate(Accountinfo_st) override { return false; }
    bool isRegistering(const std::string&) override { return false; }
    bool stageDelete(const std::string&, const std::string&) override { return false; }
    TaskSubmitResult persistAdds(std::list<Accountinfo_st>) override { return TaskSubmitResult::Stopped; }
    TaskSubmitResult persistUpdates(std::list<Accountinfo_st>) override { return TaskSubmitResult::Stopped; }
    TaskSubmitResult persistDeletes(std::list<Accountinfo_st>) override { return TaskSubmitResult::Stopped; }
    TaskSubmitResult refreshAccounts() override { return TaskSubmitResult::Stopped; }
    TaskSubmitResult autoRegister(std::string, int) override { return TaskSubmitResult::Stopped; }
    AccountAutomationSettings automationSettings() const override { return {}; }
    bool updateAutomationSettings(const AccountAutomationSettings&, std::string*) override { return false; }
};

}  // namespace

DROGON_TEST(AccountControllerInfoUsesInjectedCatalog)
{
    AccountAdminUseCaseFake catalog;
    auto visible = std::make_shared<Accountinfo_st>();
    visible->apiName = "chaynsapi";
    visible->userName = "visible-user";
    visible->status = AccountStatus::ACTIVE;
    catalog.values[visible->apiName][visible->userName] = visible;

    auto waiting = std::make_shared<Accountinfo_st>();
    waiting->apiName = "chaynsapi";
    waiting->userName = "waiting-user";
    waiting->status = AccountStatus::WAITING;
    catalog.values[waiting->apiName][waiting->userName] = waiting;

    AccountController::setUseCase(&catalog);
    drogon::HttpResponsePtr captured;
    AccountController controller;
    controller.accountInfo(
        drogon::HttpRequest::newHttpRequest(),
        [&captured](const drogon::HttpResponsePtr& response) { captured = response; });

    REQUIRE(captured != nullptr);
    const auto json = captured->getJsonObject();
    REQUIRE(json != nullptr);
    CHECK(catalog.listCalls == 1);
    CHECK(json->size() == 1);
    CHECK((*json)[0]["userName"].asString() == "visible-user");

    AccountController::setUseCase(nullptr);
}
