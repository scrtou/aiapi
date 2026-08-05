#include <drogon/drogon_test.h>

#include "sessionManager/tooling/BridgeProtocolCodec.h"

namespace {

Json::Value sampleTools() {
    Json::Value tools(Json::arrayValue);
    Json::Value tool(Json::objectValue);
    tool["type"] = "function";
    tool["function"]["name"] = "exec_command";
    tool["function"]["description"] = "Run one command";
    tool["function"]["parameters"]["type"] = "object";
    tool["function"]["parameters"]["properties"]["cmd"]["type"] = "string";
    tool["function"]["parameters"]["required"].append("cmd");
    tools.append(std::move(tool));
    return tools;
}

toolcall::BridgePolicyOptions optionsFor(const std::string& client = "Codex") {
    toolcall::BridgePolicyOptions options;
    options.clientType = client;
    options.channel = "chaynsapi";
    options.model = "test-model";
    options.sentinel = "<Function_Test123_Start/>";
    options.parallelToolCalls = false;
    return options;
}

Json::Value sampleHistory() {
    Json::Value history(Json::arrayValue);
    Json::Value assistant(Json::objectValue);
    assistant["role"] = "assistant";
    assistant["content"] = "";
    Json::Value call(Json::objectValue);
    call["id"] = "call_1";
    call["type"] = "function";
    call["function"]["name"] = "exec_command";
    call["function"]["arguments"] = "{\"cmd\":\"pwd\"}";
    assistant["tool_calls"].append(std::move(call));
    history.append(std::move(assistant));

    Json::Value result(Json::objectValue);
    result["role"] = "tool";
    result["tool_call_id"] = "call_1";
    result["content"] = "/tmp";
    history.append(std::move(result));
    return history;
}

}  // namespace

DROGON_TEST(BridgeProtocolCodec_ResolveRequestFormat)
{
    Json::Value config(Json::objectValue);
    config["format"] = "xml";
    config["format_by_channel"]["chaynsapi"] = "json";
    config["format_by_client"]["Codex"] = "xml";
    config["format_by_model"]["special"] = "json";

    CHECK(toolcall::resolveBridgeWireFormat(
              config, "Codex", "chaynsapi", "normal") ==
          toolcall::BridgeWireFormat::Xml);
    CHECK(toolcall::resolveBridgeWireFormat(
              config, "Codex", "chaynsapi", "special") ==
          toolcall::BridgeWireFormat::Json);
    CHECK(toolcall::resolveBridgeWireFormat(
              Json::Value(Json::objectValue), "Codex", "chaynsapi", "normal") ==
          toolcall::BridgeWireFormat::Json);
    CHECK(toolcall::resolveBridgeWireFormat(
              Json::Value(Json::objectValue), "RooCode", "chaynsapi", "normal") ==
          toolcall::BridgeWireFormat::Xml);
}

DROGON_TEST(BridgeProtocolCodec_FormatSpecificDefinitions)
{
    toolcall::BridgeDefinitionOptions definitionOptions;
    definitionOptions.includeDescriptions = true;
    definitionOptions.fullSchema = true;

    const auto json = toolcall::createBridgeProtocolCodec(
        toolcall::BridgeWireFormat::Json);
    const auto xml = toolcall::createBridgeProtocolCodec(
        toolcall::BridgeWireFormat::Xml);
    const std::string jsonDefinitions = json->encodeToolDefinitions(
        sampleTools(), definitionOptions);
    const std::string xmlDefinitions = xml->encodeToolDefinitions(
        sampleTools(), definitionOptions);

    CHECK(jsonDefinitions.find("\"tools\"") != std::string::npos);
    CHECK(jsonDefinitions.find("\"exec_command\"") != std::string::npos);
    CHECK(xmlDefinitions.find("<functions>") != std::string::npos);
    CHECK(xmlDefinitions.find("<name>exec_command</name>") != std::string::npos);
}

DROGON_TEST(BridgeProtocolCodec_StrictJsonDecode)
{
    const auto codec = toolcall::createBridgeProtocolCodec(
        toolcall::BridgeWireFormat::Json);
    const auto options = optionsFor();
    const auto valid = codec->decodeResponse(
        options.sentinel +
            "\n{\"protocol\":\"action-v3\",\"tool_calls\":[{\"name\":\"exec_command\",\"arguments\":{\"cmd\":\"pwd\"}}]}",
        options);
    CHECK(valid.valid);
    CHECK(valid.toolCalls.size() == 1);
    CHECK(valid.toolCalls[0].name == "exec_command");

    const auto wrongFormat = codec->decodeResponse(
        options.sentinel +
            "\n<function_calls><function_call><tool>exec_command</tool><args_json><![CDATA[{\"cmd\":\"pwd\"}]]></args_json></function_call></function_calls>",
        options);
    CHECK(!wrongFormat.valid);
}

DROGON_TEST(BridgeProtocolCodec_StrictXmlDecode)
{
    const auto codec = toolcall::createBridgeProtocolCodec(
        toolcall::BridgeWireFormat::Xml);
    const auto options = optionsFor("RooCode");
    const auto valid = codec->decodeResponse(
        options.sentinel +
            "\n<function_calls><function_call><tool>exec_command</tool><args_json><![CDATA[{\"cmd\":\"pwd\"}]]></args_json></function_call></function_calls>",
        options);
    CHECK(valid.valid);
    CHECK(valid.toolCalls.size() == 1);
    CHECK(valid.toolCalls[0].name == "exec_command");

    const auto wrongFormat = codec->decodeResponse(
        options.sentinel +
            "\n{\"protocol\":\"action-v3\",\"final_response\":\"done\"}",
        options);
    CHECK(!wrongFormat.valid);
}

DROGON_TEST(BridgeProtocolCodec_TransformsHistoryWithSameFormat)
{
    auto jsonHistory = sampleHistory();
    auto xmlHistory = sampleHistory();
    const auto options = optionsFor();

    toolcall::createBridgeProtocolCodec(toolcall::BridgeWireFormat::Json)
        ->transformHistory(jsonHistory, options);
    toolcall::createBridgeProtocolCodec(toolcall::BridgeWireFormat::Xml)
        ->transformHistory(xmlHistory, options);

    CHECK(!jsonHistory[0].isMember("tool_calls"));
    CHECK(jsonHistory[0]["content"].asString().find("action-v3") != std::string::npos);
    CHECK(jsonHistory[1]["role"].asString() == "user");
    CHECK(jsonHistory[1]["content"].asString().find("tool_result") != std::string::npos);

    CHECK(!xmlHistory[0].isMember("tool_calls"));
    CHECK(xmlHistory[0]["content"].asString().find("<function_calls>") != std::string::npos);
    CHECK(xmlHistory[1]["content"].asString().find("<tool_result") != std::string::npos);
}
