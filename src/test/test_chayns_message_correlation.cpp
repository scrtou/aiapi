#include <drogon/drogon_test.h>

#include <apipoint/chaynsapi/ChaynsMessageCorrelation.h>

#include <unordered_set>

namespace {

Json::Value message(const std::string& id,
                    const std::string& threadId,
                    const std::string& authorId,
                    int typeId,
                    const std::string& creationTime,
                    const std::string& text)
{
    Json::Value value(Json::objectValue);
    value["id"] = id;
    value["threadId"] = threadId;
    value["author"]["id"] = authorId;
    value["typeId"] = typeId;
    value["creationTime"] = creationTime;
    value["modifiedTime"] = creationTime;
    value["text"] = text;
    return value;
}

}  // namespace

DROGON_TEST(ChaynsCorrelation_SelectsFirstFinalAfterAnchor)
{
    Json::Value messages(Json::arrayValue);
    messages.append(message("reason_1", "thread", "agent", 18,
                            "2026-08-05T08:53:23.3622173Z", "thinking one"));
    messages.append(message("final_1", "thread", "agent", 1,
                            "2026-08-05T08:53:23.4586036Z", "answer one"));
    messages.append(message("user_2", "thread", "user", 1,
                            "2026-08-05T08:53:26.0036575Z", "question two"));
    messages.append(message("reason_2", "thread", "agent", 18,
                            "2026-08-05T08:53:32.6976914Z", "thinking two"));
    messages.append(message("final_2", "thread", "agent", 1,
                            "2026-08-05T08:53:32.9013111Z", "answer two"));

    chayns::MessageAnchor anchor;
    anchor.messageId = "user_1";
    anchor.threadId = "thread";
    anchor.userAuthorId = "user";
    anchor.agentAuthorId = "agent";
    anchor.creationTime = "2026-08-05T08:53:19.7743185Z";
    std::unordered_set<std::string> consumed;

    const auto result = chayns::correlateMessageBatch(messages, anchor, consumed);
    REQUIRE(result.status == chayns::CorrelationStatus::FinalFound);
    CHECK(result.finalMessage["id"].asString() == "final_1");
    REQUIRE(result.reasoningMessages.size() == 1);
    CHECK(result.reasoningMessages[0]["id"].asString() == "reason_1");
}

DROGON_TEST(ChaynsCorrelation_FindsAnchorInFullHistory)
{
    Json::Value messages(Json::arrayValue);
    messages.append(message("old_final", "thread", "agent", 1,
                            "2026-08-05T08:53:18.0000000Z", "old"));
    messages.append(message("user_1", "thread", "user", 1,
                            "2026-08-05T08:53:19.7743185Z", "question"));
    messages.append(message("reason_1", "thread", "agent", 18,
                            "2026-08-05T08:53:23.3622173Z", "thinking"));
    messages.append(message("final_1", "thread", "agent", 1,
                            "2026-08-05T08:53:23.4586036Z", "answer"));

    chayns::MessageAnchor anchor;
    anchor.messageId = "user_1";
    anchor.threadId = "thread";
    anchor.userAuthorId = "user";
    anchor.agentAuthorId = "agent";
    anchor.creationTime = "2026-08-05T08:53:19.7743185Z";
    std::unordered_set<std::string> consumed;

    const auto result = chayns::correlateMessageBatch(messages, anchor, consumed);
    REQUIRE(result.status == chayns::CorrelationStatus::FinalFound);
    CHECK(result.finalMessage["id"].asString() == "final_1");
}

DROGON_TEST(ChaynsCorrelation_StopsAtNextUserBeforeFinal)
{
    Json::Value messages(Json::arrayValue);
    messages.append(message("reason_1", "thread", "agent", 18,
                            "2026-08-05T08:53:23.3622173Z", "thinking"));
    messages.append(message("user_2", "thread", "user", 1,
                            "2026-08-05T08:53:24.0000000Z", "overlap"));
    messages.append(message("final_1", "thread", "agent", 1,
                            "2026-08-05T08:53:25.0000000Z", "ambiguous"));

    chayns::MessageAnchor anchor;
    anchor.messageId = "user_1";
    anchor.threadId = "thread";
    anchor.userAuthorId = "user";
    anchor.agentAuthorId = "agent";
    anchor.creationTime = "2026-08-05T08:53:19.7743185Z";
    std::unordered_set<std::string> consumed;

    const auto result = chayns::correlateMessageBatch(messages, anchor, consumed);
    CHECK(result.status == chayns::CorrelationStatus::Superseded);
    CHECK(result.finalMessage.isNull());
    REQUIRE(result.reasoningMessages.size() == 1);
}

DROGON_TEST(ChaynsCorrelation_DeduplicatesReasoningAcrossPolls)
{
    chayns::MessageAnchor anchor;
    anchor.messageId = "user_1";
    anchor.threadId = "thread";
    anchor.userAuthorId = "user";
    anchor.agentAuthorId = "agent";
    anchor.creationTime = "2026-08-05T08:53:19.7743185Z";
    std::unordered_set<std::string> consumed;

    Json::Value first(Json::arrayValue);
    first.append(message("reason_1", "thread", "agent", 18,
                         "2026-08-05T08:53:23.3622173Z", "thinking"));
    const auto pending = chayns::correlateMessageBatch(first, anchor, consumed);
    CHECK(pending.status == chayns::CorrelationStatus::Pending);
    REQUIRE(pending.reasoningMessages.size() == 1);

    Json::Value second = first;
    second.append(message("final_1", "thread", "agent", 1,
                          "2026-08-05T08:53:23.4586036Z", "answer"));
    const auto completed = chayns::correlateMessageBatch(second, anchor, consumed);
    REQUIRE(completed.status == chayns::CorrelationStatus::FinalFound);
    CHECK(completed.reasoningMessages.empty());
    CHECK(completed.finalMessage["id"].asString() == "final_1");
}

DROGON_TEST(ChaynsCorrelation_SortsOutOfOrderBatchChronologically)
{
    Json::Value messages(Json::arrayValue);
    messages.append(message("final_2", "thread", "agent", 1,
                            "2026-08-05T08:53:32.9013111Z", "answer two"));
    messages.append(message("user_2", "thread", "user", 1,
                            "2026-08-05T08:53:26.0036575Z", "question two"));
    messages.append(message("final_1", "thread", "agent", 1,
                            "2026-08-05T08:53:23.4586036Z", "answer one"));
    messages.append(message("reason_1", "thread", "agent", 18,
                            "2026-08-05T08:53:23.3622173Z", "thinking one"));

    chayns::MessageAnchor anchor;
    anchor.messageId = "user_1";
    anchor.threadId = "thread";
    anchor.userAuthorId = "user";
    anchor.agentAuthorId = "agent";
    anchor.creationTime = "2026-08-05T08:53:19.7743185Z";
    std::unordered_set<std::string> consumed;

    const auto result = chayns::correlateMessageBatch(messages, anchor, consumed);
    REQUIRE(result.status == chayns::CorrelationStatus::FinalFound);
    CHECK(result.finalMessage["id"].asString() == "final_1");
    REQUIRE(result.reasoningMessages.size() == 1);
    CHECK(result.reasoningMessages[0]["id"].asString() == "reason_1");
}
