#pragma once

#include <domain/model/ImageInfo.h>

#include <map>
#include <string>
#include <vector>

namespace provider {

enum class ProviderMessageRole {
    System,
    User,
    Assistant,
    Tool,
};

/** A JSON-free conversation item passed to a provider port. */
struct ProviderMessage {
    ProviderMessageRole role = ProviderMessageRole::User;
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
    // The previous local key is explicit because a provider may own an
    // upstream thread independently of application session storage.  Empty
    // means this invocation starts a new upstream conversation.
    std::string previousConversationId;
    std::string model;
    std::string systemPrompt;
    // `input` is the request-scoped prompt after application policy (for
    // example a tool bridge) has been applied. `rawInput` remains available
    // for history de-duplication without leaking a legacy session aggregate.
    std::string input;
    std::string rawInput;
    std::vector<ProviderMessage> messages;
    std::vector<ImageInfo> images;
    std::vector<ProviderToolDefinition> tools;
    std::string toolChoice;
    bool parallelToolCalls = true;
    std::string requestId;
    std::string traceId;

    /**
     * Small, string-only routing selectors supplied by the application.
     *
     * This is deliberately not the legacy JsonCpp clientInfo bag: callers
     * copy only provider-relevant, non-secret values (currently the Retool
     * `workspace_id` selector). The provider never receives the session
     * aggregate or HTTP headers merely to choose an upstream target.
     */
    std::map<std::string, std::string> routingHints;
};

}  // namespace provider
