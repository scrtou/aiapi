#ifndef CHAYNS_MESSAGE_CORRELATION_H
#define CHAYNS_MESSAGE_CORRELATION_H

#include <json/json.h>

#include <string>
#include <unordered_set>

namespace chayns {

struct MessageAnchor {
    std::string messageId;
    std::string threadId;
    std::string userAuthorId;
    std::string agentAuthorId;
    std::string creationTime;
};

enum class CorrelationStatus {
    Pending,
    FinalFound,
    Superseded
};

struct CorrelationResult {
    CorrelationStatus status = CorrelationStatus::Pending;
    Json::Value finalMessage;
    Json::Value reasoningMessages = Json::Value(Json::arrayValue);
    std::string inferredAgentAuthorId;
};

// Correlate a chronologically ordered upstream message batch with the user
// message returned by POST /message.  When the anchor itself is present (for a
// full-history query), messages before it are ignored.  For an afterDate query
// the anchor is normally absent and creationTime provides the server-side
// lower bound.
CorrelationResult correlateMessageBatch(
    const Json::Value& messages,
    const MessageAnchor& anchor,
    std::unordered_set<std::string>& consumedMessageIds);

}  // namespace chayns

#endif
