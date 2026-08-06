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

    Json::Value suggest;
    suggest["type"] = "function";
    suggest["function"]["name"] = "suggest";
    auto& parameters = suggest["function"]["parameters"];
    parameters["type"] = "object";

    Json::Value suggestRequired(Json::arrayValue);
    suggestRequired.append("actions");
    suggestRequired.append("answer");
    parameters["required"] = suggestRequired;

    auto& actions = parameters["properties"]["actions"];
    actions["type"] = "array";
    actions["minItems"] = 1;
    actions["items"]["type"] = "object";
    Json::Value itemRequired(Json::arrayValue);
    itemRequired.append("label");
    actions["items"]["required"] = itemRequired;
    actions["items"]["properties"]["label"]["type"] = "string";
    actions["items"]["properties"]["label"]["minLength"] = 1;

    parameters["properties"]["answer"]["type"] = "string";
    parameters["properties"]["answer"]["minLength"] = 1;
    tools.append(suggest);

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

DROGON_TEST(ToolCallValidator_Relaxed_SuggestRejectsEmptyActions)
{
    ToolCallValidator validator(makeToolDefs(), "Kilo-Code");
    auto result = validator.validate(
        makeToolCall("suggest", R"({"actions":[],"answer":"Apply the fix"})"),
        ValidationMode::Relaxed
    );

    CHECK(!result.valid);
}

DROGON_TEST(ToolCallValidator_Relaxed_SuggestRejectsMalformedAction)
{
    ToolCallValidator validator(makeToolDefs(), "Kilo-Code");
    auto result = validator.validate(
        makeToolCall("suggest", R"({"actions":[{}],"answer":"Apply the fix"})"),
        ValidationMode::Relaxed
    );

    CHECK(!result.valid);
}

DROGON_TEST(ToolCallValidator_Relaxed_SuggestRequiresCriticalFields)
{
    ToolCallValidator validator(makeToolDefs(), "Kilo-Code");
    auto result = validator.validate(
        makeToolCall("suggest", R"({"answer":"Apply the fix"})"),
        ValidationMode::Relaxed
    );

    CHECK(!result.valid);
}

DROGON_TEST(ToolCallValidator_Relaxed_SuggestAcceptsValidActions)
{
    ToolCallValidator validator(makeToolDefs(), "Kilo-Code");
    auto result = validator.validate(
        makeToolCall("suggest", R"({"actions":[{"label":"Apply fix"}],"answer":"Apply the fix"})"),
        ValidationMode::Relaxed
    );

    CHECK(result.valid);
}

DROGON_TEST(ToolCallValidator_Relaxed_EnumErrorIncludesActualAndAllowed)
{
    Json::Value tools(Json::arrayValue);
    Json::Value tool;
    tool["type"] = "function";
    tool["function"]["name"] = "read_file";
    auto& parameters = tool["function"]["parameters"];
    parameters["type"] = "object";
    parameters["properties"]["path"]["type"] = "string";
    auto& mode = parameters["properties"]["mode"];
    mode["type"] = "string";
    mode["enum"].append("slice");
    mode["enum"].append("indentation");
    tools.append(tool);

    ToolCallValidator validator(tools, "RooCode");
    auto result = validator.validate(
        makeToolCall("read_file", R"({"path":"README.md","mode":"read"})"),
        ValidationMode::Relaxed
    );

    CHECK(!result.valid);
    CHECK(result.errorMessage.find("actual=\"read\"") != std::string::npos);
    CHECK(result.errorMessage.find("allowed=[\"slice\",\"indentation\"]") != std::string::npos);
}

DROGON_TEST(ToolCallValidator_NamespaceLeafUsesBridgeNameAndNestedSchema)
{
    Json::Value function;
    function["type"] = "function";
    function["name"] = "read_file";
    auto& parameters = function["parameters"];
    parameters["type"] = "object";
    parameters["required"].append("path");
    parameters["properties"]["path"]["type"] = "string";

    Json::Value namespaceTool;
    namespaceTool["type"] = "namespace";
    namespaceTool["name"] = "filesystem";
    namespaceTool["tools"].append(function);

    Json::Value tools(Json::arrayValue);
    tools.append(namespaceTool);

    ToolCallValidator validator(tools, "Kilo-Code");
    CHECK(validator.hasToolDefinition("filesystem__read_file"));
    CHECK(validator.getValidToolNames().count("filesystem__read_file") == 1);

    const auto valid = validator.validate(
        makeToolCall("filesystem__read_file", R"({"path":"README.md"})"),
        ValidationMode::Strict);
    CHECK(valid.valid);

    const auto missingPath = validator.validate(
        makeToolCall("filesystem__read_file", R"({})"),
        ValidationMode::Strict);
    CHECK(!missingPath.valid);

    std::vector<generation::ToolCallDone> calls;
    calls.push_back(makeToolCall(
        "filesystem__read_file", R"({"path":"README.md"})"));
    calls.push_back(makeToolCall("filesystem__read_file", R"({})"));
    calls.push_back(makeToolCall("read_file", R"({"path":"README.md"})"));

    std::string discarded;
    const size_t removed = validator.filterInvalidToolCalls(
        calls, discarded, ValidationMode::Strict);
    CHECK(removed == 2);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].name == "filesystem__read_file");
}
