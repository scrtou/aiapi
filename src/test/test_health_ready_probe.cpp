#include <drogon/drogon_test.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <application/health/HealthUseCase.h>
#include <controllers/HealthController.h>
#include <domain/port/IAccountCatalog.h>
#include <domain/port/IAccountStore.h>
#include <domain/port/IProviderRegistry.h>

#include <list>
#include <memory>
#include <stdexcept>
#include <string>

// ARCH_TESTS: application/health/HealthUseCase.h
// ARCH_TESTS: domain/port/IHealthUseCase.h

namespace
{

class FakeAccountStore : public IAccountStore
{
  public:
    bool tableExists = true;
    bool throwOnCall = false;
    int isTableExistCalls = 0;

    bool isTableExist() override
    {
        ++isTableExistCalls;
        if (throwOnCall) throw std::runtime_error("probe boom");
        return tableExists;
    }

    bool addAccount(struct Accountinfo_st) override { return true; }
    bool updateAccount(struct Accountinfo_st) override { return true; }
    bool deleteAccount(std::string, std::string) override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    std::list<Accountinfo_st> getAccountDBList() override { return {}; }
    int createWaitingAccount(std::string) override { return 0; }
    bool activateAccount(int, struct Accountinfo_st) override { return true; }
    bool deleteWaitingAccount(int) override { return true; }
    int countAccountsByChannel(std::string, bool) override { return 0; }
    bool updateAccountStatusById(int, std::string) override { return true; }
    std::string getAccountStatusByUsername(std::string, std::string) override { return {}; }
};

class FakeProvider final : public provider::IChatProvider
{
  public:
    platform::Result<provider::ProviderResponse> generate(
        const provider::ProviderRequest&,
        provider::ProviderCallContext&) override
    {
        provider::ProviderResponse response;
        response.text = "fake";
        return platform::Result<provider::ProviderResponse>::success(std::move(response));
    }
    provider::ProviderCapabilities capabilities() const noexcept override { return {}; }
};

class FakeProviderRegistry final : public IProviderRegistry
{
  public:
    std::shared_ptr<provider::IChatProvider> provider;

    std::shared_ptr<provider::IChatProvider> findChatProvider(const std::string&) const override
    {
        return provider;
    }
};

class FakeAccountCatalog final : public IAccountCatalog
{
  public:
    AccountMap accounts;
    bool throwOnList = false;
    int listCalls = 0;

    AccountMap listAccounts() override
    {
        ++listCalls;
        if (throwOnList) throw std::runtime_error("account catalog boom");
        return accounts;
    }

    void checkChannelAccountCount(std::string) override {}
};

std::shared_ptr<HealthUseCase> installHealthUseCase(
    std::shared_ptr<IAccountStore> store,
    IProviderRegistry* providers = nullptr,
    IAccountCatalog* accounts = nullptr)
{
    auto useCase = std::make_shared<HealthUseCase>(
        std::chrono::steady_clock::now() - std::chrono::seconds(1),
        std::move(store), providers, accounts);
    HealthController::setUseCase(useCase.get());
    return useCase;
}

// 调用 /ready 并取回 checks["database"]。
int readyDatabaseFlag()
{
    drogon::HttpResponsePtr captured;
    HealthController controller;
    controller.ready(drogon::HttpRequest::newHttpRequest(),
                     [&captured](const drogon::HttpResponsePtr &resp) { captured = resp; });
    if (!captured) return -1;
    const auto json = captured->getJsonObject();
    if (!json || !json->isMember("checks") || !(*json)["checks"].isMember("database")) {
        return -1;
    }
    return (*json)["checks"]["database"].asBool() ? 1 : 0;
}

}  // namespace

DROGON_TEST(HealthReadyProbeReportsTableExists)
{
    auto store = std::make_shared<FakeAccountStore>();
    store->tableExists = true;
    auto useCase = installHealthUseCase(store);

    CHECK(readyDatabaseFlag() == 1);
    CHECK(store->isTableExistCalls == 1);

    HealthController::setUseCase(nullptr);
}

DROGON_TEST(HealthReadyProbeReportsTableMissing)
{
    auto store = std::make_shared<FakeAccountStore>();
    store->tableExists = false;
    auto useCase = installHealthUseCase(store);

    CHECK(readyDatabaseFlag() == 0);
    CHECK(store->isTableExistCalls == 1);

    HealthController::setUseCase(nullptr);
}

DROGON_TEST(HealthReadyWithoutUseCaseDegradesToFalse)
{
    HealthController::setUseCase(nullptr);
    CHECK(readyDatabaseFlag() == 0);
}

DROGON_TEST(HealthReadyUsesInjectedProviderRegistry)
{
    auto registry = std::make_unique<FakeProviderRegistry>();
    registry->provider = std::make_shared<FakeProvider>();
    auto useCase = installHealthUseCase(nullptr, registry.get());

    drogon::HttpResponsePtr captured;
    HealthController controller;
    controller.ready(drogon::HttpRequest::newHttpRequest(),
                     [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
    REQUIRE(captured != nullptr);
    const auto json = captured->getJsonObject();
    REQUIRE(json != nullptr);
    CHECK((*json)["checks"]["provider"].asBool());

    HealthController::setUseCase(nullptr);
}

DROGON_TEST(HealthReadyCountsInjectedAccountCatalog)
{
    auto catalog = std::make_unique<FakeAccountCatalog>();
    catalog->accounts["chaynsapi"]["alice"] = std::make_shared<Accountinfo_st>();
    catalog->accounts["retoolapi"]["bob"] = std::make_shared<Accountinfo_st>();
    auto useCase = installHealthUseCase(nullptr, nullptr, catalog.get());

    drogon::HttpResponsePtr captured;
    HealthController controller;
    controller.ready(drogon::HttpRequest::newHttpRequest(),
                     [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
    REQUIRE(captured != nullptr);
    const auto json = captured->getJsonObject();
    REQUIRE(json != nullptr);
    CHECK((*json)["checks"]["account"].asBool());
    CHECK((*json)["checks"]["account_count"].asUInt64() == 2);
    CHECK(catalog->listCalls == 1);

    HealthController::setUseCase(nullptr);
}

DROGON_TEST(HealthReadyContainsInjectedAccountCatalogFailure)
{
    auto catalog = std::make_unique<FakeAccountCatalog>();
    catalog->throwOnList = true;
    auto useCase = installHealthUseCase(nullptr, nullptr, catalog.get());

    drogon::HttpResponsePtr captured;
    HealthController controller;
    controller.ready(drogon::HttpRequest::newHttpRequest(),
                     [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
    REQUIRE(captured != nullptr);
    const auto json = captured->getJsonObject();
    REQUIRE(json != nullptr);
    CHECK(!(*json)["checks"]["account"].asBool());
    CHECK((*json)["checks"]["account_count"].asUInt64() == 0);

    HealthController::setUseCase(nullptr);
}

DROGON_TEST(HealthReadyProbeThrowIsContained)
{
    auto store = std::make_shared<FakeAccountStore>();
    store->throwOnCall = true;
    auto useCase = installHealthUseCase(store);

    CHECK(readyDatabaseFlag() == 0);

    HealthController::setUseCase(nullptr);
}
