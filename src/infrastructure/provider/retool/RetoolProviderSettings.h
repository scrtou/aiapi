#pragma once

#include <json/json.h>

#include <cstddef>

namespace retool {

/**
 * Immutable Retool orchestration limits supplied by the composition root.
 *
 * Keeping the parsed values here prevents the provider from reaching back
 * into Drogon's process-global application object while handling a request.
 */
struct RetoolProviderSettings
{
    std::size_t agentBootstrapSystemPromptMaxChars = 12000;
    std::size_t historyReplayMaxRequestBytes = 256 * 1024;
    std::size_t historyReplayMaxMessageBytes = 128 * 1024;
};

[[nodiscard]] RetoolProviderSettings providerSettingsFromConfig(
    const Json::Value& customConfig);

}  // namespace retool
