#include <drogon/drogon_test.h>
#include "sessionManager/tooling/ForcedToolCallGenerator.h"
using namespace toolcall;

DROGON_TEST(ForcedToolCall_KeepExisting)
{
    session_st session;
    std::vector<generation::ToolCallDone> calls;
    generation::ToolCallDone existing;
    existing.id = "call_existing";
    existing.name = "read_file";
    existing.arguments = "{}";
    calls.push_back(existing);

    std::string text = "x";
    generateForcedToolCall(session, calls, text);

    CHECK(calls.size() == 1);
    CHECK(calls[0].id == "call_existing");
}

DROGON_TEST(ForcedToolCall_NoToolDefs_UseAttemptCompletion)
{
    session_st session;
    std::vector<generation::ToolCallDone> calls;
    std::string text = "final result";

    generateForcedToolCall(session, calls, text);

    CHECK(calls.size() == 1);
    CHECK(calls[0].name == "attempt_completion");
    CHECK(calls[0].arguments.find("final result") != std::string::npos);
    CHECK(text.empty());
}

DROGON_TEST(ForcedToolCall_PickFirstToolName)
{
    session_st session;
    session.request.message = "read me";

    Json::Value tools(Json::arrayValue);
    Json::Value t;
    t["type"] = "function";
    t["function"]["name"] = "read_file";
    tools.append(t);
    session.request.tools = tools;

    std::vector<generation::ToolCallDone> calls;
    std::string text;

    generateForcedToolCall(session, calls, text);

    CHECK(calls.size() == 1);
    CHECK(calls[0].name == "read_file");
    CHECK(calls[0].arguments.find("read me") != std::string::npos);
}

DROGON_TEST(ForcedToolCall_GeneratesCallId)
{
    session_st session;
    std::vector<generation::ToolCallDone> calls;
    std::string text = "abc";

    generateForcedToolCall(session, calls, text);

    CHECK(calls.size() == 1);
    CHECK(!calls[0].id.empty());
    CHECK(calls[0].id.rfind("call_", 0) == 0);
}

DROGON_TEST(ForcedToolCall_ArgumentsLooksLikeJson)
{
    session_st session;
    std::vector<generation::ToolCallDone> calls;
    std::string text = "json text";

    generateForcedToolCall(session, calls, text);

    CHECK(calls.size() == 1);
    CHECK(calls[0].arguments.front() == '{');
    CHECK(calls[0].arguments.back() == '}');
}
