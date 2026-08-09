#include <accountManager/RetoolProvisionHealth.h>

#include <algorithm>
#include <chrono>

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

bool isRetoolProvisionCoolingDown(const RetoolProvisionHealth& state,
                                  const IRetoolProvisionClock& clock)
{
    if (state.cooldownUntil.empty()) return false;
    const auto until = clock.parseLocalTimestamp(state.cooldownUntil);
    return until.has_value() && *until > clock.now();
}

void markRetoolProvisionSuccess(IKeyValueConfigStore& store)
{
    RetoolProvisionHealth state;
    state.consecutiveFailures = 0;
    state.cooldownUntil.clear();
    persistRetoolProvisionHealth(store, state, nullptr);
}

void markRetoolProvisionFailure(IKeyValueConfigStore& store,
                                const std::string& reason,
                                const IRetoolProvisionClock& clock)
{
    auto state = loadRetoolProvisionHealth(store, nullptr);
    state.consecutiveFailures = std::max(0, state.consecutiveFailures) + 1;
    const auto now = clock.now();
    state.lastFailureAt = clock.formatLocalTimestamp(now);
    state.lastFailureReason = reason;
    if (state.consecutiveFailures >= kRetoolFailureThreshold)
    {
        state.cooldownUntil = clock.formatLocalTimestamp(
            now + std::chrono::minutes(kRetoolCooldownMinutes));
    }
    persistRetoolProvisionHealth(store, state, nullptr);
}

}  // namespace retoolProvision
