#include <infrastructure/provider/retool/RetoolProviderSettings.h>

#include <algorithm>
#include <cstdint>

namespace {

constexpr std::size_t kMaximumConfiguredBytes = 8 * 1024 * 1024;

std::size_t boundedHistoryValue(const Json::Value& value,
                                std::size_t fallback)
{
    std::uint64_t configured = fallback;
    if (value.isUInt64()) {
        configured = value.asUInt64();
    } else if (value.isInt()) {
        const int signedValue = value.asInt();
        if (signedValue < 0) return fallback;
        configured = static_cast<std::uint64_t>(signedValue);
    } else {
        return fallback;
    }
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        configured, static_cast<std::uint64_t>(kMaximumConfiguredBytes)));
}

std::size_t bootstrapValue(const Json::Value& value, std::size_t fallback)
{
    if (value.isUInt64()) return static_cast<std::size_t>(value.asUInt64());
    if (!value.isInt()) return fallback;
    const int configured = value.asInt();
    return configured <= 0 ? 0 : static_cast<std::size_t>(configured);
}

}  // namespace

namespace retool {

RetoolProviderSettings providerSettingsFromConfig(
    const Json::Value& customConfig)
{
    RetoolProviderSettings settings;
    if (!customConfig.isObject()) return settings;

    const auto& provider = customConfig["retoolapi"];
    if (provider.isObject() &&
        provider.isMember("agent_bootstrap_system_prompt_max_chars")) {
        settings.agentBootstrapSystemPromptMaxChars = bootstrapValue(
            provider["agent_bootstrap_system_prompt_max_chars"],
            settings.agentBootstrapSystemPromptMaxChars);
    }

    const auto& replay = customConfig["history_replay"];
    if (replay.isObject()) {
        settings.historyReplayMaxRequestBytes = boundedHistoryValue(
            replay["max_request_bytes"],
            settings.historyReplayMaxRequestBytes);
        settings.historyReplayMaxMessageBytes = boundedHistoryValue(
            replay["max_message_bytes"],
            settings.historyReplayMaxMessageBytes);
    }
    return settings;
}

}  // namespace retool
