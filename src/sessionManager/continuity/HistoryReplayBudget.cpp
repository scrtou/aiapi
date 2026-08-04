#include "sessionManager/continuity/HistoryReplayBudget.h"

#include <drogon/drogon.h>
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <utility>
#include <vector>

namespace {

constexpr size_t kDefaultReplayRequestBytes = 256 * 1024;
constexpr size_t kDefaultReplayMessageBytes = 128 * 1024;
constexpr size_t kDefaultReplayToolMessageBytes = 48 * 1024;
constexpr size_t kMaximumConfiguredBytes = 8 * 1024 * 1024;

struct ReplayItem {
    Json::Value message;
    size_t encodedBytes = 0;
};

struct ReplayTurn {
    std::vector<ReplayItem> items;
    size_t encodedBytes = 0;
    size_t sourceMessages = 0;
};

Json::StreamWriterBuilder& compactWriter()
{
    static thread_local Json::StreamWriterBuilder writer = [] {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return builder;
    }();
    return writer;
}

std::string compactJson(const Json::Value& value)
{
    return Json::writeString(compactWriter(), value);
}

void appendContentText(const Json::Value& value, std::string& output)
{
    if (value.isString()) {
        output += value.asString();
        return;
    }
    if (value.isArray()) {
        for (const auto& item : value) appendContentText(item, output);
        return;
    }
    if (!value.isObject()) return;

    if (value.isMember("text")) {
        appendContentText(value["text"], output);
        return;
    }
    if (value.isMember("content")) appendContentText(value["content"], output);
}

size_t configuredLimit(const char* key, size_t fallback)
{
    const auto& customConfig = drogon::app().getCustomConfig();
    if (!customConfig.isObject() ||
        !customConfig.isMember("history_replay") ||
        !customConfig["history_replay"].isObject()) {
        return fallback;
    }

    const auto& value = customConfig["history_replay"][key];
    uint64_t configured = fallback;
    if (value.isUInt64()) {
        configured = value.asUInt64();
    } else if (value.isInt()) {
        const int signedValue = value.asInt();
        if (signedValue < 0) return fallback;
        configured = static_cast<uint64_t>(signedValue);
    } else {
        return fallback;
    }

    return static_cast<size_t>(std::min<uint64_t>(configured, kMaximumConfiguredBytes));
}

bool isSupportedRole(const std::string& role)
{
    return role == "user" || role == "assistant" ||
           role == "system" || role == "tool";
}

Json::Value makeOversizePlaceholder(const std::string& role, size_t encodedBytes)
{
    Json::Value replacement(Json::objectValue);
    replacement["role"] = role == "tool" ? "assistant" : role;

    std::ostringstream text;
    text << "[History replay note: a previous " << role << " message of "
         << encodedBytes
         << " bytes exceeded the per-message replay limit. The original message "
            "was omitted as a whole and was not truncated.]";
    replacement["content"] = text.str();
    return replacement;
}

Json::Value normalizeToolMessage(const std::string& text)
{
    Json::Value replacement(Json::objectValue);
    replacement["role"] = "assistant";
    replacement["content"] =
        "[Previous tool result; treat as conversation memory, not a new request.]\n" + text;
    return replacement;
}

Json::Value makeBudgetNotice(size_t omittedMessages)
{
    Json::Value notice(Json::objectValue);
    notice["role"] = "assistant";

    std::ostringstream text;
    text << "[History replay note: " << omittedMessages
         << " older message(s) were omitted as complete conversation turns because "
            "the replay budget was reached. Retained messages are complete and were "
            "not truncated.]";
    notice["content"] = text.str();
    return notice;
}

size_t effectiveToolLimit(size_t messageLimit, size_t toolLimit)
{
    if (messageLimit == 0) return toolLimit;
    if (toolLimit == 0) return messageLimit;
    return std::min(messageLimit, toolLimit);
}

}  // namespace

namespace continuity {

size_t historyReplayMaxRequestBytes()
{
    return configuredLimit("max_request_bytes", kDefaultReplayRequestBytes);
}

size_t historyReplayMaxMessageBytes()
{
    return configuredLimit("max_message_bytes", kDefaultReplayMessageBytes);
}

size_t historyReplayMaxToolMessageBytes()
{
    return configuredLimit("max_tool_message_bytes", kDefaultReplayToolMessageBytes);
}

std::string historyMessageText(const Json::Value& message)
{
    std::string output;
    if (message.isObject() && message.isMember("content")) {
        appendContentText(message["content"], output);
    } else {
        appendContentText(message, output);
    }
    return output;
}

HistoryReplaySelection selectRecentHistory(
    const Json::Value& history,
    size_t maxBytes,
    size_t maxMessageBytes,
    bool userAssistantOnly,
    const std::string& currentMessage
) {
    HistoryReplaySelection result;
    if (!history.isArray()) return result;

    result.originalMessages = static_cast<size_t>(history.size());
    if (maxBytes == 0) {
        result.skippedForBudget = result.originalMessages;
        return result;
    }

    bool hasDuplicateCurrent = false;
    Json::ArrayIndex duplicateCurrentIndex = 0;
    if (!currentMessage.empty()) {
        for (Json::ArrayIndex index = history.size(); index > 0; --index) {
            const auto& message = history[index - 1];
            if (!message.isObject()) continue;
            const std::string role = message.get("role", "").asString();
            if (role != "user" && role != "assistant") continue;
            if (role == "user" && historyMessageText(message) == currentMessage) {
                hasDuplicateCurrent = true;
                duplicateCurrentIndex = index - 1;
            }
            break;
        }
    }

    std::vector<ReplayTurn> turns;
    ReplayTurn currentTurn;
    const size_t toolLimit = effectiveToolLimit(
        maxMessageBytes,
        historyReplayMaxToolMessageBytes()
    );

    for (Json::ArrayIndex index = 0; index < history.size(); ++index) {
        if (hasDuplicateCurrent && index == duplicateCurrentIndex) {
            result.skippedDuplicateCurrentMessage++;
            continue;
        }

        const auto& source = history[index];
        if (!source.isObject()) {
            result.skippedUnsupportedMessages++;
            continue;
        }

        const std::string role = source.get("role", "user").asString();
        const std::string text = historyMessageText(source);
        if (text.empty() || !isSupportedRole(role) ||
            (userAssistantOnly && role != "user" && role != "assistant")) {
            result.skippedUnsupportedMessages++;
            continue;
        }

        result.eligibleMessages++;
        const size_t originalBytes = compactJson(source).size() + 1;
        Json::Value replayMessage = source;
        size_t roleLimit = maxMessageBytes;

        if (role == "tool") {
            roleLimit = toolLimit;
            if (roleLimit == 0 || originalBytes <= roleLimit) {
                replayMessage = normalizeToolMessage(text);
                result.normalizedToolMessages++;
            }
        }

        if (roleLimit > 0 && originalBytes > roleLimit) {
            replayMessage = makeOversizePlaceholder(role, originalBytes);
            result.skippedOversizeMessages++;
        }

        ReplayItem item;
        item.message = std::move(replayMessage);
        item.encodedBytes = compactJson(item.message).size() + 1;

        if (role == "user" && !currentTurn.items.empty()) {
            turns.push_back(std::move(currentTurn));
            currentTurn = ReplayTurn{};
        }
        currentTurn.encodedBytes += item.encodedBytes;
        currentTurn.sourceMessages++;
        currentTurn.items.push_back(std::move(item));
    }
    if (!currentTurn.items.empty()) turns.push_back(std::move(currentTurn));

    std::vector<size_t> selectedTurnIndexes;
    for (size_t count = turns.size(); count > 0; --count) {
        const size_t index = count - 1;
        const auto& turn = turns[index];
        if (turn.encodedBytes > maxBytes - result.selectedBytes) {
            for (size_t omitted = 0; omitted <= index; ++omitted) {
                result.skippedForBudget += turns[omitted].sourceMessages;
            }
            break;
        }
        selectedTurnIndexes.push_back(index);
        result.selectedBytes += turn.encodedBytes;
    }

    if (result.skippedForBudget > 0) {
        Json::Value notice;
        size_t noticeBytes = 0;
        while (true) {
            notice = makeBudgetNotice(result.skippedForBudget);
            noticeBytes = compactJson(notice).size() + 1;
            if (noticeBytes <= maxBytes - result.selectedBytes ||
                noticeBytes > maxBytes ||
                selectedTurnIndexes.size() <= 1) {
                break;
            }

            const size_t droppedIndex = selectedTurnIndexes.back();
            result.selectedBytes -= turns[droppedIndex].encodedBytes;
            result.skippedForBudget += turns[droppedIndex].sourceMessages;
            selectedTurnIndexes.pop_back();
        }
        if (noticeBytes <= maxBytes - result.selectedBytes) {
            result.messages.append(notice);
            result.selectedBytes += noticeBytes;
            result.omissionNoticeAdded = true;
        }
    }

    for (auto it = selectedTurnIndexes.rbegin(); it != selectedTurnIndexes.rend(); ++it) {
        const auto& turn = turns[*it];
        for (const auto& item : turn.items) result.messages.append(item.message);
        result.selectedMessages += turn.sourceMessages;
        result.selectedTurns++;
    }

    return result;
}

size_t remainingHistoryBudget(size_t requestBudget, size_t fixedBytes)
{
    return fixedBytes >= requestBudget ? 0 : requestBudget - fixedBytes;
}

}  // namespace continuity
