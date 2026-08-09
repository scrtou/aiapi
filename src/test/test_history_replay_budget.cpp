#include <drogon/drogon_test.h>
#include <sessionManager/continuity/HistoryReplayBudget.h>

using namespace continuity;

namespace {

Json::Value message(const std::string& role, const std::string& content)
{
    Json::Value value(Json::objectValue);
    value["role"] = role;
    value["content"] = content;
    return value;
}

size_t encodedBytes(const Json::Value& value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value).size() + 1;
}

}  // namespace

DROGON_TEST(HistoryReplayBudget_KeepsRecentCompleteSuffix)
{
    Json::Value history(Json::arrayValue);
    history.append(message("user", std::string(100, 'a')));
    history.append(message("assistant", std::string(100, 'b')));
    history.append(message("user", "latest"));

    const auto selected = selectRecentHistory(history, 80, 1000);

    CHECK(selected.selectedMessages == 1);
    CHECK(selected.selectedTurns == 1);
    CHECK(selected.messages.size() == 1);
    CHECK(selected.messages[0]["content"].asString() == "latest");
    CHECK(selected.skippedForBudget == 2);
}

DROGON_TEST(HistoryReplayBudget_PreservesChronologicalOrder)
{
    Json::Value history(Json::arrayValue);
    history.append(message("user", "one"));
    history.append(message("assistant", "two"));
    history.append(message("user", "three"));

    const auto selected = selectRecentHistory(history, 4096, 4096);

    CHECK(selected.messages.size() == 3);
    CHECK(selected.messages[0]["content"].asString() == "one");
    CHECK(selected.messages[1]["content"].asString() == "two");
    CHECK(selected.messages[2]["content"].asString() == "three");
    CHECK(selected.selectedTurns == 2);
}

DROGON_TEST(HistoryReplayBudget_ReplacesOversizedMessageWithoutTruncating)
{
    Json::Value history(Json::arrayValue);
    history.append(message("user", "small question"));
    history.append(message("assistant", std::string(500, 'x')));

    const auto selected = selectRecentHistory(history, 4096, 100);

    CHECK(selected.skippedOversizeMessages == 1);
    CHECK(selected.messages.size() == 2);
    CHECK(selected.messages[0]["content"].asString() == "small question");
    CHECK(selected.messages[1]["role"].asString() == "assistant");
    CHECK(selected.messages[1]["content"].asString().find("was not truncated") != std::string::npos);
    CHECK(selected.messages[1]["content"].asString() != std::string(500, 'x'));
}

DROGON_TEST(HistoryReplayBudget_DoesNotSplitConversationTurnAtBoundary)
{
    Json::Value history(Json::arrayValue);
    history.append(message("user", std::string(120, 'q')));
    history.append(message("assistant", std::string(120, 'a')));
    history.append(message("user", "recent question"));
    history.append(message("assistant", "recent answer"));

    const size_t recentTurnBytes = encodedBytes(history[2]) + encodedBytes(history[3]);
    const auto selected = selectRecentHistory(history, recentTurnBytes, 4096);

    CHECK(selected.selectedMessages == 2);
    CHECK(selected.selectedTurns == 1);
    CHECK(selected.messages.size() == 2);
    CHECK(selected.messages[0]["content"].asString() == "recent question");
    CHECK(selected.messages[1]["content"].asString() == "recent answer");
    CHECK(selected.skippedForBudget == 2);
}

DROGON_TEST(HistoryReplayBudget_AddsNoticeWhenOlderTurnsAreOmitted)
{
    Json::Value history(Json::arrayValue);
    history.append(message("user", std::string(300, 'q')));
    history.append(message("assistant", std::string(300, 'a')));
    history.append(message("user", "recent question"));
    history.append(message("assistant", "recent answer"));

    const auto selected = selectRecentHistory(history, 400, 4096);

    CHECK(selected.omissionNoticeAdded);
    CHECK(selected.selectedMessages == 2);
    CHECK(selected.messages.size() == 3);
    CHECK(selected.messages[0]["content"].asString().find("older message(s)") != std::string::npos);
    CHECK(selected.messages[1]["content"].asString() == "recent question");
    CHECK(selected.messages[2]["content"].asString() == "recent answer");
}

DROGON_TEST(HistoryReplayBudget_NormalizesToolHistoryAsAssistantMemory)
{
    Json::Value history(Json::arrayValue);
    history.append(message("user", "question"));
    history.append(message("tool", "tool output"));
    history.append(message("assistant", "answer"));

    const auto selected = selectRecentHistory(history, 4096, 4096);

    CHECK(selected.normalizedToolMessages == 1);
    CHECK(selected.messages.size() == 3);
    CHECK(selected.messages[1]["role"].asString() == "assistant");
    CHECK(selected.messages[1]["content"].asString().find("Previous tool result") != std::string::npos);
}

DROGON_TEST(HistoryReplayBudget_CanFilterReplayRoles)
{
    Json::Value history(Json::arrayValue);
    history.append(message("system", "system"));
    history.append(message("tool", "tool output"));
    history.append(message("user", "question"));
    history.append(message("assistant", "answer"));

    const auto selected = selectRecentHistory(history, 4096, 4096, true);

    CHECK(selected.messages.size() == 2);
    CHECK(selected.messages[0]["role"].asString() == "user");
    CHECK(selected.messages[1]["role"].asString() == "assistant");
    CHECK(selected.skippedUnsupportedMessages == 2);
}

DROGON_TEST(HistoryReplayBudget_SkipsTrailingDuplicateCurrentUser)
{
    Json::Value history(Json::arrayValue);
    history.append(message("user", "older question"));
    history.append(message("assistant", "older answer"));
    history.append(message("user", "current question"));

    const auto selected = selectRecentHistory(
        history,
        4096,
        4096,
        false,
        "current question"
    );

    CHECK(selected.messages.size() == 2);
    CHECK(selected.messages[0]["content"].asString() == "older question");
    CHECK(selected.messages[1]["content"].asString() == "older answer");
    CHECK(selected.skippedDuplicateCurrentMessage == 1);
}

DROGON_TEST(HistoryReplayBudget_ZeroBudgetDisablesHistory)
{
    Json::Value history(Json::arrayValue);
    history.append(message("user", "question"));

    const auto selected = selectRecentHistory(history, 0, 4096);

    CHECK(selected.messages.empty());
    CHECK(selected.skippedForBudget == 1);
    CHECK(remainingHistoryBudget(100, 100) == 0);
    CHECK(remainingHistoryBudget(100, 40) == 60);
}
