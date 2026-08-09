#ifndef DOMAIN_MODEL_SESSION_DATA_H
#define DOMAIN_MODEL_SESSION_DATA_H

#include <ctime>
#include <string>

enum class SessionTrackingMode {
    Hash,
    ZeroWidth
};

enum class ApiType {
    ChatCompletions,
    Responses
};

inline constexpr int SESSION_EXPIRE_TIME = 86400;
inline constexpr int SESSION_CLEANUP_INTERVAL = 3600;

// Pure session lifecycle state shared by continuity and persistence policies.
// Protocol payloads and provider-specific data stay in edge-owned contracts.
struct SessionState
{
    ApiType apiType = ApiType::ChatCompletions;
    bool hasPreviousResponseId = false;
    bool isContinuation = false;
    std::string conversationId;
    std::string nextSessionId;
    std::time_t createdAt = 0;
    std::time_t lastActiveAt = 0;
    std::string requestId;
    std::string contextConversationId;
    int contextLength = 0;
    bool contextIsFull = false;
};

#endif  // DOMAIN_MODEL_SESSION_DATA_H
