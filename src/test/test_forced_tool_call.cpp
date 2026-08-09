#include <drogon/drogon_test.h>
#include "sessionManager/tooling/ForcedToolCallGenerator.h"
using namespace toolcall;

namespace {

Json::Value toolDefinitions()
{
    Json::Value tools(Json::arrayValue);
    Json::Value tool(Json::objectValue);
    tool["type"] = "function";
    tool["function"]["name"] = "read_file";
    tool["function"]["parameters"]["type"] = "object";
    tool["function"]["parameters"]["properties"]["path"]["type"] = "string";
    tool["function"]["parameters"]["required"].append("path");
    tools.append(tool);
    return tools;
}

session_st requiredToolSession()
{
    session_st session;
    session.request.toolChoice = "required";
    session.request.tools = toolDefinitions();
    return session;
}

}  // namespace

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

DROGON_TEST(ForcedToolCall_NoToolDefs_DoesNotInventTool)
{
    session_st session;
    session.request.toolChoice = "required";
    std::vector<generation::ToolCallDone> calls;
    std::string text = "final result";

    generateForcedToolCall(session, calls, text);

    CHECK(calls.empty());
    CHECK(text == "final result");
}

DROGON_TEST(ForcedToolCall_PickFirstToolName)
{
    session_st session = requiredToolSession();
    session.request.message = "read me";

    std::vector<generation::ToolCallDone> calls;
    std::string text;

    generateForcedToolCall(session, calls, text);

    REQUIRE(calls.size() == 1);
    CHECK(calls[0].name == "read_file");
    CHECK(calls[0].arguments.find("read me") != std::string::npos);
}

DROGON_TEST(ForcedToolCall_GeneratesCallId)
{
    session_st session = requiredToolSession();
    std::vector<generation::ToolCallDone> calls;
    std::string text = "abc";

    generateForcedToolCall(session, calls, text);

    REQUIRE(calls.size() == 1);
    CHECK(!calls[0].id.empty());
    CHECK(calls[0].id.rfind("call_", 0) == 0);
}

DROGON_TEST(ForcedToolCall_ArgumentsLooksLikeJson)
{
    session_st session = requiredToolSession();
    std::vector<generation::ToolCallDone> calls;
    std::string text = "json text";

    generateForcedToolCall(session, calls, text);

    REQUIRE(calls.size() == 1);
    CHECK(calls[0].arguments.front() == '{');
    CHECK(calls[0].arguments.back() == '}');
}
