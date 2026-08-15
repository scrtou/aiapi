#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace provider {

/** Token usage reported by a successful provider response. */
struct Usage {
    int inputTokens = 0;
    int outputTokens = 0;
    int totalTokens = 0;

    bool isValid() const { return totalTokens > 0; }
};

/** A native tool invocation returned by a successful provider response. */
struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

// Provider metadata is deliberately a small, transport-neutral string map.
// JSON materialization belongs to an edge codec; domain code must not carry
// JsonCpp types across the provider/application boundary.
using ProviderMetadata = std::map<std::string, std::string>;

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
