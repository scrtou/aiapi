#include "RetoolProvisionHealth.h"

#include <drogon/drogon.h>
#include <algorithm>
#include <chrono>
#include <ctime>

using namespace drogon;

namespace retoolProvision {

constexpr const char* kRetoolFailureCountKey = "retoolapi.provision.consecutive_failures";
constexpr const char* kRetoolLastFailureAtKey = "retoolapi.provision.last_failure_at";
constexpr const char* kRetoolLastFailureReasonKey = "retoolapi.provision.last_failure_reason";
constexpr const char* kRetoolCooldownUntilKey = "retoolapi.provision.cooldown_until";
constexpr int kRetoolFailureThreshold = 3;
constexpr int kRetoolCooldownMinutes = 30;

RetoolProvisionHealth loadRetoolProvisionHealth(IKeyValueConfigStore& store, std::string* errorMessage)
{
    RetoolProvisionHealth state;
    store.ensureTable(errorMessage);
    if (auto value = store.getValue(kRetoolFailureCountKey, nullptr); value && !value->empty()) {
        try { state.consecutiveFailures = std::stoi(*value); } catch (...) {}
    }
    if (auto value = store.getValue(kRetoolLastFailureAtKey, nullptr); value) {
        state.lastFailureAt = *value;
    }
    if (auto value = store.getValue(kRetoolLastFailureReasonKey, nullptr); value) {
        state.lastFailureReason = *value;
    }
    if (auto value = store.getValue(kRetoolCooldownUntilKey, nullptr); value) {
        state.cooldownUntil = *value;
    }
    return state;
}

bool persistRetoolProvisionHealth(IKeyValueConfigStore& store,
                                  const RetoolProvisionHealth& state,
                                  std::string* errorMessage)
{
    return store.setValues({
        {kRetoolFailureCountKey, std::to_string(std::max(0, state.consecutiveFailures))},
        {kRetoolLastFailureAtKey, state.lastFailureAt},
        {kRetoolLastFailureReasonKey, state.lastFailureReason},
        {kRetoolCooldownUntilKey, state.cooldownUntil},
    }, errorMessage);
}

bool isRetoolProvisionCoolingDown(const RetoolProvisionHealth& state)
{
    if (state.cooldownUntil.empty()) return false;
    try
    {
        auto untilDate = trantor::Date::fromDbStringLocal(state.cooldownUntil);
        return untilDate.secondsSinceEpoch() > trantor::Date::now().secondsSinceEpoch();
    }
    catch (...)
    {
        return false;
    }
}

void markRetoolProvisionSuccess(IKeyValueConfigStore& store)
{
    RetoolProvisionHealth state;
    state.consecutiveFailures = 0;
    state.cooldownUntil.clear();
    persistRetoolProvisionHealth(store, state, nullptr);
}

void markRetoolProvisionFailure(IKeyValueConfigStore& store, const std::string& reason)
{
    auto state = loadRetoolProvisionHealth(store, nullptr);
    state.consecutiveFailures = std::max(0, state.consecutiveFailures) + 1;
    state.lastFailureAt = trantor::Date::now().toDbStringLocal();
    state.lastFailureReason = reason;
    if (state.consecutiveFailures >= kRetoolFailureThreshold)
    {
        auto cooldownUntil = trantor::Date::date().after(static_cast<double>(kRetoolCooldownMinutes) * 60.0);
        state.cooldownUntil = cooldownUntil.toDbStringLocal();
    }
    persistRetoolProvisionHealth(store, state, nullptr);
}

}  // namespace retoolProvision
