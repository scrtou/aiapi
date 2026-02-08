#include <drogon/drogon_test.h>
#include "sessionManager/tooling/ToolCallValidator.h"
using namespace toolcall;

namespace {

Json::Value makeToolDefs() {
    Json::Value tools(Json::arrayValue);

    Json::Value tool;
    tool["type"] = "function";
    tool["function"]["name"] = "write_to_file";
    tool["function"]["parameters"]["type"] = "object";

    Json::Value required(Json::arrayValue);
    required.append("path");
    required.append("content");
    tool["function"]["parameters"]["required"] = required;

    tool["function"]["parameters"]["properties"]["path"]["type"] = "string";
    tool["function"]["parameters"]["properties"]["content"]["type"] = "string";

    tools.append(tool);
    return tools;
}

generation::ToolCallDone makeToolCall(const std::string& name, const std::string& args) {
    generation::ToolCallDone tc;
    tc.id = "call_1";
    tc.name = name;
    tc.arguments = args;
    tc.index = 0;
    return tc;
}

}

DROGON_TEST(ToolCallValidator_ValidateStrict_Valid)
{
    ToolCallValidator validator(makeToolDefs(), "Kilo-Code");
    auto result = validator.validate(
        makeToolCall("write_to_file", R"({"path":"a.txt","content":"hello"})"),
        ValidationMode::Strict
    );

    CHECK(result.valid);
}

DROGON_TEST(ToolCallValidator_ValidateStrict_UnknownTool)
{
    ToolCallValidator validator(makeToolDefs(), "Kilo-Code");
    auto result = validator.validate(
        makeToolCall("unknown_tool", R"({"x":1})"),
        ValidationMode::Strict
    );

    CHECK(!result.valid);
    CHECK(!result.errorMessage.empty());
}

DROGON_TEST(ToolCallValidator_ValidateStrict_InvalidJson)
{
    ToolCallValidator validator(makeToolDefs(), "Kilo-Code");
    auto result = validator.validate(
        makeToolCall("write_to_file", "{invalid-json"),
        ValidationMode::Strict
    );

    CHECK(!result.valid);
}

DROGON_TEST(ToolCallValidator_FilterInvalidToolCalls)
{
    ToolCallValidator validator(makeToolDefs(), "Kilo-Code");

    std::vector<generation::ToolCallDone> calls;
    calls.push_back(makeToolCall("write_to_file", R"({"path":"a.txt","content":"ok"})"));
    calls.push_back(makeToolCall("write_to_file", R"({"path":"","content":"bad"})"));
    calls.push_back(makeToolCall("unknown", R"({"x":1})"));

    std::string discarded;
    size_t removed = validator.filterInvalidToolCalls(calls, discarded, ValidationMode::Strict);

    CHECK(removed >= 1);
    CHECK(calls.size() == 1);
    CHECK(calls[0].name == "write_to_file");
}

DROGON_TEST(ToolCallValidator_Relaxed_CriticalFieldEmpty)
{
    ToolCallValidator validator(makeToolDefs(), "RooCode");

    auto result = validator.validate(
        makeToolCall("write_to_file", R"({"path":"","content":"x"})"),
        ValidationMode::Relaxed
    );

    CHECK(!result.valid);
}
