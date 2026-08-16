#include <drogon/drogon_test.h>
#include <application/generation/actionProtocol/ActionProtocolCompiler.h>
#include <application/generation/actionProtocol/ActionProtocolAdapter.h>
#include <sstream>

using namespace actionproto;

namespace {
std::string buildRouterPolicyFor(const std::string& clientType, bool parallel) {
    return ActionProtocolCompiler::buildRouterPolicy(
        "<S/>", capabilitiesForClient(clientType, parallel));
}
} // namespace

DROGON_TEST(ActionProtocol_CompilesJsonToolCall)
{
    const std::string sentinel = "<Function_Json123_Start/>";
    const std::string input = sentinel + R"(
{"protocol":"action-v3","tool_calls":[{"name":"exec_command","arguments":{"cmd":"printf '%s\\n' ']]>'","workdir":"/tmp"}}]})";

    CompileOptions options;
    options.expectedSentinel = sentinel;
    options.capabilities.maxToolCalls = 1;
    const auto result = ActionProtocolCompiler::compileResponse(input, options);
    REQUIRE(result.matched);
    REQUIRE(result.valid);
    CHECK(result.envelope.protocolVersion == 3);
    REQUIRE(result.envelope.toolCalls.size() == 1);
    CHECK(result.envelope.toolCalls[0].name == "exec_command");
    // Action parsing is client/wire-output agnostic.  The response pipeline
    // assigns the correlation ID only after all tool-call sources are merged.
    CHECK(result.envelope.toolCalls[0].id.empty());

    Json::Value arguments;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(result.envelope.toolCalls[0].argumentsJson);
    REQUIRE(Json::parseFromStream(builder, stream, &arguments, &errors));
    CHECK(arguments["cmd"].asString().find("]]>" ) != std::string::npos);
    CHECK(arguments["workdir"].asString() == "/tmp");
}

DROGON_TEST(ActionProtocol_CompilesJsonFinalResponse)
{
    const std::string sentinel = "<Function_Json456_Start/>";
    const std::string input = sentinel + R"(
{"protocol":"action-v3","final_response":"first line\nsecond line with <xml> and ]]>"})";

    CompileOptions options;
    options.expectedSentinel = sentinel;
    const auto result = ActionProtocolCompiler::compileResponse(input, options);
    REQUIRE(result.valid);
    CHECK(result.envelope.protocolVersion == 3);
    CHECK(result.envelope.toolCalls.empty());
    CHECK(result.envelope.finalResponse == "first line\nsecond line with <xml> and ]]>");
}

DROGON_TEST(ActionProtocol_RejectsJsonEncodedArgumentsString)
{
    const std::string sentinel = "<Function_JsonBad_Start/>";
    const std::string input = sentinel + R"(
{"protocol":"action-v3","tool_calls":[{"name":"read_file","arguments":"{\"path\":\"x\"}"}]})";

    CompileOptions options;
    options.expectedSentinel = sentinel;
    const auto result = ActionProtocolCompiler::compileResponse(input, options);
    CHECK(result.matched);
    CHECK(!result.valid);
    CHECK(result.diagnostic.code == CompileError::InvalidArgumentsJson);
}

DROGON_TEST(ActionProtocol_RejectsAmbiguousJsonAction)
{
    const std::string sentinel = "<Function_JsonBoth_Start/>";
    const std::string input = sentinel + R"(
{"protocol":"action-v3","tool_calls":[],"final_response":"done"})";

    CompileOptions options;
    options.expectedSentinel = sentinel;
    const auto result = ActionProtocolCompiler::compileResponse(input, options);
    CHECK(result.matched);
    CHECK(!result.valid);
    CHECK(result.diagnostic.code == CompileError::MissingAction);
}

DROGON_TEST(ActionProtocol_RouterPolicyUsesJsonOnly)
{
    const std::string policy = ActionProtocolCompiler::buildRouterPolicy(
        "<Function_Policy_Start/>", capabilitiesForClient("Codex", false));
    CHECK(policy.find("action-v3") != std::string::npos);
    CHECK(policy.find("\"arguments\":{") != std::string::npos);
    CHECK(policy.find("<![CDATA[") == std::string::npos);
    CHECK(policy.find("<action_protocol") == std::string::npos);
}

DROGON_TEST(ActionProtocol_CompilesToolCall)
{
    const std::string sentinel = "<Function_Test123_Start/>";
    const std::string input = sentinel + R"(
<action_protocol version="1">
<tool_calls>
  <tool_call>
    <name>exec_command</name>
    <arguments_json><![CDATA[{"cmd":"printf hello"}]]></arguments_json>
  </tool_call>
</tool_calls>
</action_protocol>
<end_action/>)";

    CompileOptions options;
    options.expectedSentinel = sentinel;
    options.capabilities.maxToolCalls = 1;
    const auto result = ActionProtocolCompiler::compileResponse(input, options);
    REQUIRE(result.matched);
    REQUIRE(result.valid);
    REQUIRE(result.envelope.toolCalls.size() == 1);
    CHECK(result.envelope.toolCalls[0].name == "exec_command");
    CHECK(result.envelope.toolCalls[0].argumentsJson.find("printf hello") != std::string::npos);
}

DROGON_TEST(ActionProtocol_CompilesFinalResponseWithEmbeddedXml)
{
    const std::string sentinel = "<Function_Test456_Start/>";
    const std::string input = sentinel + R"(
<action_protocol version="1">
<final_response><![CDATA[Here is an XML example:
<function_calls><function_call><tool>read_file</tool></function_call></function_calls>
Finished.]]></final_response>
</action_protocol>
<end_action/>)";

    CompileOptions options;
    options.expectedSentinel = sentinel;
    const auto result = ActionProtocolCompiler::compileResponse(input, options);
    REQUIRE(result.valid);
    CHECK(result.envelope.toolCalls.empty());
    CHECK(result.envelope.finalResponse.find("<function_calls>") != std::string::npos);
    CHECK(result.envelope.finalResponse.find("Finished.") != std::string::npos);
}

DROGON_TEST(ActionProtocol_RejectsEmptyToolName)
{
    const std::string sentinel = "<Function_Test789_Start/>";
    const std::string input = sentinel + R"(
<action_protocol version="1"><tool_calls>
<tool_call><name></name><arguments_json><![CDATA[{}]]></arguments_json></tool_call>
</tool_calls></action_protocol><end_action/>)";

    CompileOptions options;
    options.expectedSentinel = sentinel;
    const auto result = ActionProtocolCompiler::compileResponse(input, options);
    CHECK(result.matched);
    CHECK(!result.valid);
    CHECK(result.diagnostic.code == CompileError::InvalidActionShape);
}

DROGON_TEST(ActionProtocol_RejectsMalformedJson)
{
    const std::string sentinel = "<Function_TestBad_Start/>";
    const std::string input = sentinel + R"(
<action_protocol version="1"><tool_calls>
<tool_call><name>read_file</name><arguments_json><![CDATA[{"path":"x"]]></arguments_json></tool_call>
</tool_calls></action_protocol><end_action/>)";

    CompileOptions options;
    options.expectedSentinel = sentinel;
    const auto result = ActionProtocolCompiler::compileResponse(input, options);
    CHECK(result.matched);
    CHECK(!result.valid);
    CHECK(result.diagnostic.code == CompileError::InvalidArgumentsJson);
}

DROGON_TEST(ActionProtocol_RejectsUnexpectedTrailingText)
{
    const std::string sentinel = "<Function_TestTail_Start/>";
    const std::string input = sentinel + R"(
<action_protocol version="1"><tool_calls>
<tool_call><name>read_file</name><arguments_json><![CDATA[{}]]></arguments_json></tool_call>
</tool_calls></action_protocol><end_action/> trailing prose)";

    CompileOptions options;
    options.expectedSentinel = sentinel;
    const auto result = ActionProtocolCompiler::compileResponse(input, options);
    CHECK(result.matched);
    CHECK(!result.valid);
    CHECK(result.diagnostic.code == CompileError::InvalidEnvelope);
}

DROGON_TEST(ActionProtocol_ClientCapabilities)
{
    const auto codex = capabilitiesForClient("Codex", false);
    CHECK(codex.maxToolCalls == 1);
    const auto codexParallel = capabilitiesForClient("Codex", true);
    CHECK(codexParallel.maxToolCalls > 1);
    const auto roo = capabilitiesForClient("RooCode", true);
    CHECK(roo.requiresActionEveryTurn);
    CHECK(roo.maxToolCalls == 1);
    CHECK(roo.supportsFinalText == false);
    CHECK(roo.supportsParallelCalls == false);
}

DROGON_TEST(ActionProtocol_RejectsMoreToolCallsThanClientAllows)
{
    CompileOptions options;
    options.expectedSentinel = "<S/>";
    options.wireFormat = WireFormat::JsonV3;
    options.capabilities = capabilitiesForClient("RooCode", true);

    const std::string twoCalls =
        "<S/>{\"protocol\":\"action-v3\",\"tool_calls\":["
        "{\"name\":\"a\",\"arguments\":{}},"
        "{\"name\":\"b\",\"arguments\":{}}]}";
    const auto rejected =
        ActionProtocolCompiler::compileResponse(twoCalls, options);
    CHECK(rejected.matched);
    CHECK(rejected.valid == false);
    CHECK(rejected.diagnostic.code == CompileError::MultipleActions);

    const std::string oneCall =
        "<S/>{\"protocol\":\"action-v3\",\"tool_calls\":["
        "{\"name\":\"a\",\"arguments\":{}}]}";
    const auto accepted =
        ActionProtocolCompiler::compileResponse(oneCall, options);
    CHECK(accepted.valid);
    REQUIRE(accepted.envelope.toolCalls.size() == 1);
}

DROGON_TEST(ActionProtocol_PolicyStatesExactlyOneActionContract)
{
    const auto roo = buildRouterPolicyFor("RooCode", true);
    CHECK(roo.find("Every response MUST contain exactly 1 action.") != std::string::npos);
    CHECK(roo.find("Never emit both, and never emit neither.") != std::string::npos);
    CHECK(roo.find("final_response is converted into the client completion tool call.")
          != std::string::npos);

    const auto codex = buildRouterPolicyFor("Codex", true);
    CHECK(codex.find("Every response MUST contain exactly 1 action.") != std::string::npos);
    CHECK(codex.find("final_response is converted into") == std::string::npos);
}

DROGON_TEST(ActionProtocol_AdapterKeepsClientSpecificCompletionAtEdge)
{
    ActionEnvelope envelope;
    envelope.finalResponse = "done";
    const auto codex = adaptForCapabilities(
        envelope, capabilitiesForClient("Codex", false));
    CHECK(codex.toolCalls.empty());
    CHECK(codex.text == "done");

    const auto roo = adaptForCapabilities(
        envelope, capabilitiesForClient("RooCode", false));
    REQUIRE(roo.toolCalls.size() == 1);
    CHECK(roo.toolCalls[0].name == "attempt_completion");
    CHECK(roo.toolCalls[0].id.empty());
    CHECK(roo.text.empty());
}
