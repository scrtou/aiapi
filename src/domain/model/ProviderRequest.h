#pragma once

#include <domain/model/ImageInfo.h>

#include <string>
#include <vector>

namespace provider {

enum class MessageRole {
    System,
    User,
    Assistant,
    Tool,
};

/** A JSON-free conversation item passed to a provider port. */
struct ProviderMessage {
    MessageRole role = MessageRole::User;
    std::string text;
    std::string toolCallId;
};

/**
 * Tool schema stays serialized until the protocol-specific codec is migrated.
 * This preserves the domain boundary without prematurely introducing a generic
 * JsonCpp replacement into P6-W1.
 */
struct ProviderToolDefinition {
    std::string name;
    std::string description;
    std::string parametersJson;
};

/** Immutable provider invocation input; no session_st or HTTP types leak in. */
struct ProviderRequest {
    std::string conversationId;
    std::string model;
    std::string systemPrompt;
    std::vector<ProviderMessage> messages;
    std::vector<ImageInfo> images;
    std::vector<ProviderToolDefinition> tools;
    std::string toolChoice;
    bool parallelToolCalls = true;
    std::string requestId;
    std::string traceId;
};

}  // namespace provider
