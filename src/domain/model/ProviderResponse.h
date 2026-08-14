#pragma once

#include <domain/model/ProviderResult.h>

#include <optional>
#include <string>
#include <vector>

namespace provider {

/**
 * Successful provider output.  Failure is represented by Result<>, never by
 * an error field embedded in this response.
 */
struct ProviderResponse {
    std::string text;
    std::optional<Usage> usage;
    std::vector<ToolCall> toolCalls;
    ProviderMetadata meta;
};

}  // namespace provider
