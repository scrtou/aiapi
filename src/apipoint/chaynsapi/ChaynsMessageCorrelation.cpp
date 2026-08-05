#include "ChaynsMessageCorrelation.h"

#include <algorithm>
#include <vector>

namespace chayns {
namespace {

std::string stringField(const Json::Value& value, const char* name)
{
    return value.isObject() && value.isMember(name) && value[name].isString()
        ? value[name].asString()
        : std::string();
}

std::string authorId(const Json::Value& message)
{
    if (!message.isObject() || !message.isMember("author") ||
        !message["author"].isObject()) {
        return {};
    }
    return stringField(message["author"], "id");
}

// Chayns timestamps are UTC ISO-8601 values.  Normalize their fractional
// seconds so lexical ordering remains chronological when precision differs.
std::string timestampSortKey(const std::string& timestamp)
{
    const auto dot = timestamp.find('.');
    const auto zone = timestamp.find('Z', dot == std::string::npos ? 0 : dot);
    if (dot == std::string::npos || zone == std::string::npos) {
        return timestamp;
    }

    std::string key = timestamp.substr(0, dot + 1);
    std::string fraction = timestamp.substr(dot + 1, zone - dot - 1);
    fraction.resize(9, '0');
    key += fraction.substr(0, 9);
    key += timestamp.substr(zone);
    return key;
}

}  // namespace

CorrelationResult correlateMessageBatch(
    const Json::Value& messages,
    const MessageAnchor& anchor,
    std::unordered_set<std::string>& consumedMessageIds)
{
    CorrelationResult result;
    result.inferredAgentAuthorId = anchor.agentAuthorId;
    if (!messages.isArray()) {
        return result;
    }

    std::vector<const Json::Value*> ordered;
    ordered.reserve(messages.size());
    bool batchContainsAnchor = false;
    for (const auto& message : messages) {
        if (!message.isObject()) {
            continue;
        }
        ordered.push_back(&message);
        if (!anchor.messageId.empty() && stringField(message, "id") == anchor.messageId) {
            batchContainsAnchor = true;
        }
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return timestampSortKey(stringField(*left, "creationTime")) <
               timestampSortKey(stringField(*right, "creationTime"));
    });

    bool anchorSeen = !batchContainsAnchor;
    const std::string anchorTimeKey = timestampSortKey(anchor.creationTime);
    for (const auto* messagePtr : ordered) {
        const auto& message = *messagePtr;
        const std::string messageId = stringField(message, "id");
        const std::string messageThreadId = stringField(message, "threadId");
        const std::string messageCreationTime = stringField(message, "creationTime");

        if (!anchor.threadId.empty() && messageThreadId != anchor.threadId) {
            continue;
        }
        if (!anchorSeen) {
            if (messageId == anchor.messageId) {
                anchorSeen = true;
            }
            continue;
        }
        if (!anchorTimeKey.empty() &&
            timestampSortKey(messageCreationTime) <= anchorTimeKey &&
            messageId != anchor.messageId) {
            continue;
        }
        if (messageId == anchor.messageId) {
            continue;
        }
        if (!messageId.empty() && consumedMessageIds.count(messageId) > 0) {
            continue;
        }

        const std::string messageAuthorId = authorId(message);
        if (!anchor.userAuthorId.empty() && messageAuthorId == anchor.userAuthorId) {
            result.status = CorrelationStatus::Superseded;
            return result;
        }

        const int typeId = message.get("typeId", 0).asInt();
        if (typeId != 1 && typeId != 18) {
            if (!messageId.empty()) {
                consumedMessageIds.insert(messageId);
            }
            continue;
        }

        if (result.inferredAgentAuthorId.empty() &&
            !messageAuthorId.empty() && messageAuthorId != anchor.userAuthorId) {
            result.inferredAgentAuthorId = messageAuthorId;
        }
        if (messageAuthorId.empty() ||
            (!result.inferredAgentAuthorId.empty() &&
             messageAuthorId != result.inferredAgentAuthorId)) {
            continue;
        }

        if (!messageId.empty()) {
            consumedMessageIds.insert(messageId);
        }
        if (typeId == 18) {
            result.reasoningMessages.append(message);
            continue;
        }

        result.finalMessage = message;
        result.status = CorrelationStatus::FinalFound;
        return result;
    }

    return result;
}

}  // namespace chayns
