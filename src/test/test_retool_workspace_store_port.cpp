#include <drogon/drogon_test.h>

#include <dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h>
#include <domain/port/IRetoolWorkspaceStore.h>
#include <retoolWorkspace/RetoolWorkspaceManager.h>
#include <retoolWorkspace/RetoolWorkspaceService.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// R4 依赖倒置试点的可测性取证：
// 通过 IRetoolWorkspaceStore 注入内存 Fake，使 RetoolWorkspaceManager
// 可在完全不接触数据库/drogon DbClient 的情况下被断言。
// 改造前该类硬依赖 RetoolWorkspaceDbManager 单例，本文件无法存在。

namespace
{

class FakeRetoolWorkspaceStore : public IRetoolWorkspaceStore
{
  public:
    std::vector<RetoolWorkspaceInfo> rows;
    int ensureTableCalls = 0;
    std::vector<std::string> usageLog;

    bool ensureTable(std::string*) override
    {
        ++ensureTableCalls;
        return true;
    }

    bool upsertWorkspace(const RetoolWorkspaceInfo& info, std::string*) override
    {
        for (auto& r : rows)
        {
            if (r.workspaceId == info.workspaceId)
            {
                r = info;
                return true;
            }
        }
        rows.push_back(info);
        return true;
    }

    bool deleteWorkspace(const std::string& id, std::string*) override
    {
        for (auto it = rows.begin(); it != rows.end(); ++it)
        {
            if (it->workspaceId == id)
            {
                rows.erase(it);
                return true;
            }
        }
        return false;
    }

    std::optional<RetoolWorkspaceInfo> getWorkspace(const std::string& id, std::string*) override
    {
        for (const auto& r : rows)
        {
            if (r.workspaceId == id)
            {
                return r;
            }
        }
        return std::nullopt;
    }

    std::vector<RetoolWorkspaceInfo> listWorkspaces(std::string*) override { return rows; }

    bool updateWorkspaceStatus(const std::string& id,
                               const std::string& status,
                               const std::string& verifyStatus,
                               std::string*) override
    {
        for (auto& r : rows)
        {
            if (r.workspaceId == id)
            {
                r.status = status;
                r.verifyStatus = verifyStatus;
                return true;
            }
        }
        return false;
    }

    bool updateWorkspaceUsage(const std::string& id, int inUseCount, bool touchLastUsedAt, std::string*) override
    {
        usageLog.push_back(id + ":" + std::to_string(inUseCount) + (touchLastUsedAt ? ":touch" : ":notouch"));
        // 真正回写，使 in-use 计数语义可被断言（而非只验证"被调用过"）。
        for (auto& r : rows)
        {
            if (r.workspaceId == id)
            {
                r.inUseCount = inUseCount;
                return true;
            }
        }
        return false;
    }
};

struct ManagerFixture
{
    ManagerFixture()
        : store(std::make_shared<FakeRetoolWorkspaceStore>()), manager(store)
    {
    }

    std::shared_ptr<FakeRetoolWorkspaceStore> store;
    RetoolWorkspaceManager manager;
};

}  // namespace

DROGON_TEST(RetoolWorkspacePort_InitEnsuresTableThroughInjectedStore)
{
    ManagerFixture fixture;
    fixture.manager.init();
    CHECK(fixture.store->ensureTableCalls == 1);
}

DROGON_TEST(RetoolWorkspacePort_UpsertThenGetRoundTrips)
{
    ManagerFixture fixture;

    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-round-trip";
    CHECK(fixture.manager.upsertWorkspace(info, nullptr));

    auto got = fixture.manager.getWorkspace("ws-round-trip", nullptr);
    REQUIRE(got.has_value());
    CHECK(got->workspaceId == "ws-round-trip");
    CHECK(fixture.store->rows.size() == 1);
}

DROGON_TEST(RetoolWorkspacePort_GetMissingReturnsNullopt)
{
    ManagerFixture fixture;
    auto got = fixture.manager.getWorkspace("no-such-id", nullptr);
    CHECK(!got.has_value());
}

DROGON_TEST(RetoolWorkspacePort_DeleteRemovesRow)
{
    ManagerFixture fixture;

    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-delete";
    fixture.manager.upsertWorkspace(info, nullptr);
    CHECK(fixture.store->rows.size() == 1);

    CHECK(fixture.manager.deleteWorkspace("ws-delete", nullptr));
    CHECK(fixture.store->rows.empty());
}

DROGON_TEST(RetoolWorkspacePort_UpdateStatusIsForwarded)
{
    ManagerFixture fixture;

    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-status";
    fixture.manager.upsertWorkspace(info, nullptr);

    CHECK(fixture.manager.updateWorkspaceStatus("ws-status", "active", "verified", nullptr));
    REQUIRE(fixture.store->rows.size() == 1);
    CHECK(fixture.store->rows[0].status == "active");
    CHECK(fixture.store->rows[0].verifyStatus == "verified");
}

DROGON_TEST(RetoolWorkspacePort_UsageCounterIncrementsThenDecrements)
{
    ManagerFixture fixture;

    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-usage";
    info.inUseCount = 0;
    fixture.manager.upsertWorkspace(info, nullptr);

    CHECK(fixture.manager.markWorkspaceUsageStarted("ws-usage", nullptr));
    REQUIRE(fixture.store->rows.size() == 1);
    CHECK(fixture.store->rows[0].inUseCount == 1);

    CHECK(fixture.manager.markWorkspaceUsageFinished("ws-usage", nullptr));
    CHECK(fixture.store->rows[0].inUseCount == 0);

    // 两次都应带 touchLastUsedAt=true
    REQUIRE(fixture.store->usageLog.size() == 2);
    CHECK(fixture.store->usageLog[0] == "ws-usage:1:touch");
    CHECK(fixture.store->usageLog[1] == "ws-usage:0:touch");
}

DROGON_TEST(RetoolWorkspacePort_UsageCounterNeverGoesNegative)
{
    ManagerFixture fixture;

    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-floor";
    info.inUseCount = 0;
    fixture.manager.upsertWorkspace(info, nullptr);

    // 未开始就结束：实现用 std::max(0, n-1) 兜底，不应变负。
    CHECK(fixture.manager.markWorkspaceUsageFinished("ws-floor", nullptr));
    REQUIRE(fixture.store->rows.size() == 1);
    CHECK(fixture.store->rows[0].inUseCount == 0);
}

DROGON_TEST(RetoolWorkspacePort_UsageOnMissingWorkspaceShortCircuits)
{
    ManagerFixture fixture;

    // 工作区不存在时应提前返回 false，且不产生任何 usage 写入。
    CHECK(!fixture.manager.markWorkspaceUsageStarted("no-such-id", nullptr));
    CHECK(!fixture.manager.markWorkspaceUsageFinished("no-such-id", nullptr));
    CHECK(fixture.store->usageLog.empty());
}

DROGON_TEST(RetoolWorkspacePort_NullStoreFailsSafelyWithDiagnostic)
{
    // 未注入实现时必须返回失败并给出错误信息，而不是解引用空指针。
    RetoolWorkspaceManager manager(nullptr);

    std::string error;
    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-null";

    CHECK(!manager.upsertWorkspace(info, &error));
    CHECK(!error.empty());
    CHECK(manager.listWorkspaces(nullptr).empty());
    CHECK(!manager.getWorkspace("ws-null", nullptr).has_value());
}

DROGON_TEST(RetoolWorkspaceProvisionerRequiresInjectedStore)
{
    bool rejected = false;
    try
    {
        RetoolWorkspaceService provisioner(std::shared_ptr<IRetoolWorkspaceStore>{});
        static_cast<void>(provisioner);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    CHECK(rejected);
}

/*
 * The concrete adapter may no longer lazily locate Drogon's DB client through
 * a singleton accessor.  Before AppWiring explicitly initializes it, fail
 * diagnosably instead of dereferencing a null client or silently constructing
 * another process-wide owner.
 */
DROGON_TEST(RetoolWorkspaceDbStoreRequiresExplicitRuntimeInitialization)
{
    RetoolWorkspaceDbManager store;
    std::string error;

    CHECK(!store.ensureTable(&error));
    CHECK(!error.empty());
}
