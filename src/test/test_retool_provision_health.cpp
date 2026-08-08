#include <drogon/drogon_test.h>

#include <accountManager/RetoolProvisionHealth.h>

#include <map>
#include <optional>
#include <string>

// Retool 开通健康度状态机的取证测试。
// 这些用例在 step 167 之前无法存在：该状态机原本硬依赖 ConfigDbManager 单例，
// 断言三振阈值/冷却窗口就必须连真库。经 IKeyValueConfigStore 倒置后，
// 用内存 Fake 即可完全离库地断言。

namespace
{

class FakeKeyValueConfigStore : public IKeyValueConfigStore
{
  public:
    std::map<std::string, std::string> rows;
    int ensureTableCalls = 0;
    int setValuesCalls = 0;

    bool ensureTable(std::string*) override
    {
        ++ensureTableCalls;
        return true;
    }

    std::optional<std::string> getValue(const std::string& key, std::string*) override
    {
        auto it = rows.find(key);
        if (it == rows.end()) return std::nullopt;
        return it->second;
    }

    bool setValues(const std::map<std::string, std::string>& entries, std::string*) override
    {
        ++setValuesCalls;
        for (const auto& kv : entries) rows[kv.first] = kv.second;
        return true;
    }
};

const char* kFailureCountKey = "retoolapi.provision.consecutive_failures";
const char* kLastFailureReasonKey = "retoolapi.provision.last_failure_reason";
const char* kCooldownUntilKey = "retoolapi.provision.cooldown_until";

}  // namespace

DROGON_TEST(RetoolProvisionHealthLoadDefaults)
{
    FakeKeyValueConfigStore store;
    const auto state = retoolProvision::loadRetoolProvisionHealth(store, nullptr);
    CHECK(store.ensureTableCalls == 1);
    CHECK(state.consecutiveFailures == 0);
    CHECK(state.cooldownUntil.empty());
    CHECK(state.lastFailureReason.empty());
}

DROGON_TEST(RetoolProvisionHealthLoadToleratesGarbageCounter)
{
    FakeKeyValueConfigStore store;
    store.rows[kFailureCountKey] = "not-a-number";
    const auto state = retoolProvision::loadRetoolProvisionHealth(store, nullptr);
    // stoi 抛出被吞掉，计数退回 0 而不是让调用方炸掉。
    CHECK(state.consecutiveFailures == 0);
}

DROGON_TEST(RetoolProvisionHealthFailuresBelowThresholdDoNotCoolDown)
{
    FakeKeyValueConfigStore store;
    retoolProvision::markRetoolProvisionFailure(store, "boom-1");
    CHECK(store.rows[kFailureCountKey] == "1");
    CHECK(store.rows[kCooldownUntilKey].empty());

    retoolProvision::markRetoolProvisionFailure(store, "boom-2");
    CHECK(store.rows[kFailureCountKey] == "2");
    CHECK(store.rows[kCooldownUntilKey].empty());

    const auto state = retoolProvision::loadRetoolProvisionHealth(store, nullptr);
    CHECK(retoolProvision::isRetoolProvisionCoolingDown(state) == false);
    CHECK(state.lastFailureReason == "boom-2");
}

DROGON_TEST(RetoolProvisionHealthThirdFailureOpensCooldown)
{
    FakeKeyValueConfigStore store;
    retoolProvision::markRetoolProvisionFailure(store, "a");
    retoolProvision::markRetoolProvisionFailure(store, "b");
    retoolProvision::markRetoolProvisionFailure(store, "c");

    CHECK(store.rows[kFailureCountKey] == "3");
    CHECK(!store.rows[kCooldownUntilKey].empty());

    const auto state = retoolProvision::loadRetoolProvisionHealth(store, nullptr);
    CHECK(retoolProvision::isRetoolProvisionCoolingDown(state) == true);
    CHECK(store.rows[kLastFailureReasonKey] == "c");
}

DROGON_TEST(RetoolProvisionHealthSuccessClearsCounterAndCooldown)
{
    FakeKeyValueConfigStore store;
    retoolProvision::markRetoolProvisionFailure(store, "a");
    retoolProvision::markRetoolProvisionFailure(store, "b");
    retoolProvision::markRetoolProvisionFailure(store, "c");
    REQUIRE(!store.rows[kCooldownUntilKey].empty());

    retoolProvision::markRetoolProvisionSuccess(store);

    CHECK(store.rows[kFailureCountKey] == "0");
    CHECK(store.rows[kCooldownUntilKey].empty());
    const auto state = retoolProvision::loadRetoolProvisionHealth(store, nullptr);
    CHECK(retoolProvision::isRetoolProvisionCoolingDown(state) == false);
}

DROGON_TEST(RetoolProvisionHealthPastCooldownStampIsNotCoolingDown)
{
    retoolProvision::RetoolProvisionHealth state;
    state.cooldownUntil = "2000-01-01 00:00:00";
    CHECK(retoolProvision::isRetoolProvisionCoolingDown(state) == false);
}

DROGON_TEST(RetoolProvisionHealthUnparsableCooldownStampIsNotCoolingDown)
{
    retoolProvision::RetoolProvisionHealth state;
    state.cooldownUntil = "garbage";
    CHECK(retoolProvision::isRetoolProvisionCoolingDown(state) == false);
}
