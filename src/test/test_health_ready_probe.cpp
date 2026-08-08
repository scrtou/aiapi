#include <drogon/drogon_test.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <controllers/HealthController.h>
#include <domain/port/IAccountStore.h>

#include <list>
#include <memory>
#include <stdexcept>
#include <string>

// 步骤 176 的配套回归测试：/ready 的库探针改走 IAccountStore 端口后，
// HealthController 首次获得测试覆盖（此前 aiapi_test 里该类符号数为 0，见步骤 179.3）。
//
// 断言点刻意选在 checks["database"] 这一格，而不是 status 总判：
// 离线测试环境下 provider/account 两项必为 false，status 恒 "not_ready"，
// 用它断言会把探针的对错整个掩盖掉。
//
// 覆盖边界：只覆盖 HealthController 与 IAccountStore 之间的契约。
// main.cc 的启动接线不在此列（本测试自行注入），由
// tools/arch/check_startup_wiring.py 的 REQUIRED_STATIC 分支守住。

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
        if (throwOnCall)
        {
            throw std::runtime_error("probe boom");
        }
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

// 调用 /ready 并取回 checks["database"]。
// 返回 -1 表示响应体缺失或该字段不存在（与 true/false 区分开，避免误判为通过）。
int readyDatabaseFlag()
{
    drogon::HttpResponsePtr captured;
    HealthController controller;
    controller.ready(drogon::HttpRequest::newHttpRequest(),
                     [&captured](const drogon::HttpResponsePtr &resp) { captured = resp; });
    if (!captured)
    {
        return -1;
    }
    const auto json = captured->getJsonObject();
    if (!json || !json->isMember("checks") || !(*json)["checks"].isMember("database"))
    {
        return -1;
    }
    return (*json)["checks"]["database"].asBool() ? 1 : 0;
}

}  // namespace

DROGON_TEST(HealthReadyProbeReportsTableExists)
{
    auto store = std::make_shared<FakeAccountStore>();
    store->tableExists = true;
    HealthController::setDbProbe(store);

    CHECK(readyDatabaseFlag() == 1);
    // 探针必须真的被调用过——否则「恒返回 true」的实现也能骗过上一条断言。
    CHECK(store->isTableExistCalls == 1);

    HealthController::setDbProbe(nullptr);
}

DROGON_TEST(HealthReadyProbeReportsTableMissing)
{
    auto store = std::make_shared<FakeAccountStore>();
    store->tableExists = false;
    HealthController::setDbProbe(store);

    CHECK(readyDatabaseFlag() == 0);
    CHECK(store->isTableExistCalls == 1);

    HealthController::setDbProbe(nullptr);
}

// 漏注入时的降级语义：不崩溃，database 判 false。
// 这正是 check_startup_wiring.py 要拦的那种「静默故障」，此处固化其行为。
DROGON_TEST(HealthReadyWithoutProbeDegradesToFalse)
{
    HealthController::setDbProbe(nullptr);
    CHECK(readyDatabaseFlag() == 0);
}

// 探针抛异常时必须被吞掉并判 false，而不是把异常泄漏到 drogon 的请求处理链。
DROGON_TEST(HealthReadyProbeThrowIsContained)
{
    auto store = std::make_shared<FakeAccountStore>();
    store->throwOnCall = true;
    HealthController::setDbProbe(store);

    CHECK(readyDatabaseFlag() == 0);

    HealthController::setDbProbe(nullptr);
}
