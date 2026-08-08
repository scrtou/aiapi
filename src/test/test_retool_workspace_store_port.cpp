#include <drogon/drogon_test.h>

#include <domain/port/IRetoolWorkspaceStore.h>
#include <retoolWorkspace/RetoolWorkspaceManager.h>

#include <memory>
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

std::shared_ptr<FakeRetoolWorkspaceStore> installFake()
{
    auto fake = std::make_shared<FakeRetoolWorkspaceStore>();
    RetoolWorkspaceManager::getInstance().setStore(fake);
    return fake;
}

}  // namespace

DROGON_TEST(RetoolWorkspacePort_InitEnsuresTableThroughInjectedStore)
{
    auto fake = installFake();
    RetoolWorkspaceManager::getInstance().init();
    CHECK(fake->ensureTableCalls == 1);
}

DROGON_TEST(RetoolWorkspacePort_UpsertThenGetRoundTrips)
{
    auto fake = installFake();

    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-round-trip";
    CHECK(RetoolWorkspaceManager::getInstance().upsertWorkspace(info, nullptr));

    auto got = RetoolWorkspaceManager::getInstance().getWorkspace("ws-round-trip", nullptr);
    REQUIRE(got.has_value());
    CHECK(got->workspaceId == "ws-round-trip");
    CHECK(fake->rows.size() == 1);
}

DROGON_TEST(RetoolWorkspacePort_GetMissingReturnsNullopt)
{
    installFake();
    auto got = RetoolWorkspaceManager::getInstance().getWorkspace("no-such-id", nullptr);
    CHECK(!got.has_value());
}

DROGON_TEST(RetoolWorkspacePort_DeleteRemovesRow)
{
    auto fake = installFake();

    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-delete";
    RetoolWorkspaceManager::getInstance().upsertWorkspace(info, nullptr);
    CHECK(fake->rows.size() == 1);

    CHECK(RetoolWorkspaceManager::getInstance().deleteWorkspace("ws-delete", nullptr));
    CHECK(fake->rows.empty());
}

DROGON_TEST(RetoolWorkspacePort_UpdateStatusIsForwarded)
{
    auto fake = installFake();

    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-status";
    RetoolWorkspaceManager::getInstance().upsertWorkspace(info, nullptr);

    CHECK(RetoolWorkspaceManager::getInstance().updateWorkspaceStatus("ws-status", "active", "verified", nullptr));
    REQUIRE(fake->rows.size() == 1);
    CHECK(fake->rows[0].status == "active");
    CHECK(fake->rows[0].verifyStatus == "verified");
}

DROGON_TEST(RetoolWorkspacePort_UsageCounterIncrementsThenDecrements)
{
    auto fake = installFake();

    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-usage";
    info.inUseCount = 0;
    RetoolWorkspaceManager::getInstance().upsertWorkspace(info, nullptr);

    CHECK(RetoolWorkspaceManager::getInstance().markWorkspaceUsageStarted("ws-usage", nullptr));
    REQUIRE(fake->rows.size() == 1);
    CHECK(fake->rows[0].inUseCount == 1);

    CHECK(RetoolWorkspaceManager::getInstance().markWorkspaceUsageFinished("ws-usage", nullptr));
    CHECK(fake->rows[0].inUseCount == 0);

    // 两次都应带 touchLastUsedAt=true
    REQUIRE(fake->usageLog.size() == 2);
    CHECK(fake->usageLog[0] == "ws-usage:1:touch");
    CHECK(fake->usageLog[1] == "ws-usage:0:touch");
}

DROGON_TEST(RetoolWorkspacePort_UsageCounterNeverGoesNegative)
{
    auto fake = installFake();

    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-floor";
    info.inUseCount = 0;
    RetoolWorkspaceManager::getInstance().upsertWorkspace(info, nullptr);

    // 未开始就结束：实现用 std::max(0, n-1) 兜底，不应变负。
    CHECK(RetoolWorkspaceManager::getInstance().markWorkspaceUsageFinished("ws-floor", nullptr));
    REQUIRE(fake->rows.size() == 1);
    CHECK(fake->rows[0].inUseCount == 0);
}

DROGON_TEST(RetoolWorkspacePort_UsageOnMissingWorkspaceShortCircuits)
{
    auto fake = installFake();

    // 工作区不存在时应提前返回 false，且不产生任何 usage 写入。
    CHECK(!RetoolWorkspaceManager::getInstance().markWorkspaceUsageStarted("no-such-id", nullptr));
    CHECK(!RetoolWorkspaceManager::getInstance().markWorkspaceUsageFinished("no-such-id", nullptr));
    CHECK(fake->usageLog.empty());
}

DROGON_TEST(RetoolWorkspacePort_NullStoreFailsSafelyWithDiagnostic)
{
    // 未注入实现时必须返回失败并给出错误信息，而不是解引用空指针。
    RetoolWorkspaceManager::getInstance().setStore(nullptr);

    std::string error;
    RetoolWorkspaceInfo info;
    info.workspaceId = "ws-null";

    CHECK(!RetoolWorkspaceManager::getInstance().upsertWorkspace(info, &error));
    CHECK(!error.empty());
    CHECK(RetoolWorkspaceManager::getInstance().listWorkspaces(nullptr).empty());
    CHECK(!RetoolWorkspaceManager::getInstance().getWorkspace("ws-null", nullptr).has_value());
}
