#pragma once

#include <string_view>

namespace retired_provider {

inline constexpr std::string_view kNexosProviderKey = "nexosapi";
inline constexpr std::string_view kDirectOpenAiKey = "openai";

/// Runtime/provider keys that must never be registered, loaded or re-created.
inline bool isRetiredProviderKey(std::string_view key)
{
    return key == kNexosProviderKey || key == kDirectOpenAiKey;
}

/// Historical config used "nexos" while runtime used "nexosapi".
inline bool isRetiredProviderConfigKey(std::string_view key)
{
    return isRetiredProviderKey(key) || key == "nexos";
}

}  // namespace retired_provider
