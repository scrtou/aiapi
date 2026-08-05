#include <drogon/drogon_test.h>
#include "sessionManager/actionProtocol/ActionProtocolCompiler.h"
#include "sessionManager/actionProtocol/ActionProtocolAdapter.h"

using namespace actionproto;

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
}

DROGON_TEST(ActionProtocol_AdapterKeepsClientSpecificCompletionAtEdge)
{
    ActionEnvelope envelope;
    envelope.finalResponse = "done";
    const auto codex = adaptForClient(envelope, "Codex");
    CHECK(codex.toolCalls.empty());
    CHECK(codex.text == "done");

    const auto roo = adaptForClient(envelope, "RooCode");
    REQUIRE(roo.toolCalls.size() == 1);
    CHECK(roo.toolCalls[0].name == "attempt_completion");
    CHECK(roo.text.empty());
}
