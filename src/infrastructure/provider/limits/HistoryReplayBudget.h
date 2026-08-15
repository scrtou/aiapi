#ifndef HISTORY_REPLAY_BUDGET_H
#define HISTORY_REPLAY_BUDGET_H

#include <json/json.h>
#include <cstddef>
#include <string>

namespace continuity {

struct HistoryReplaySelection {
    Json::Value messages{Json::arrayValue};
    size_t originalMessages = 0;
    size_t eligibleMessages = 0;
    size_t selectedMessages = 0;
    size_t selectedTurns = 0;
    size_t selectedBytes = 0;
    size_t skippedOversizeMessages = 0;
    size_t skippedForBudget = 0;
    size_t skippedUnsupportedMessages = 0;
    size_t skippedDuplicateCurrentMessage = 0;
    size_t normalizedToolMessages = 0;
    bool omissionNoticeAdded = false;
};

size_t historyReplayMaxRequestBytes();
size_t historyReplayMaxMessageBytes();
size_t historyReplayMaxToolMessageBytes();
std::string historyMessageText(const Json::Value& message);

HistoryReplaySelection selectRecentHistory(
    const Json::Value& history,
    size_t maxBytes,
    size_t maxMessageBytes,
    bool userAssistantOnly = false,
    const std::string& currentMessage = ""
);

size_t remainingHistoryBudget(size_t requestBudget, size_t fixedBytes);

}  // namespace continuity

#endif
