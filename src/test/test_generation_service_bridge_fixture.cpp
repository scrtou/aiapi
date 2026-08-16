#include <drogon/drogon_test.h>

#include <domain/port/IChannelCatalog.h>
#include <infrastructure/provider/ProviderRegistry.h>
#include <application/generation/continuity/ResponseIndex.h>
#include <application/generation/core/SessionExecutionGate.h>
#include <application/generation/core/Session.h>
#include <application/generation/core/SessionCodec.h>
#include <application/generation/contracts/IResponseSink.h>
#include <application/generation/core/GenerationService.h>
#include <platform/ZeroWidthEncoder.h>

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeChannelCatalog final : public IChannelCatalog
{
  public:
    std::list<Channelinfo_st> rows;
    std::list<Channelinfo_st> listChannels() const override { return rows; }
    bool addChannel(Channelinfo_st row) override { rows.push_back(std::move(row)); return true; }
    bool updateChannel(Channelinfo_st) override { return true; }
    bool deleteChannel(int) override { return true; }
    bool updateChannelStatus(std::string, bool) override { return true; }
    std::optional<bool> supportsToolCalls(const std::string& name) const override
    { for (const auto& row : rows) if (row.channelName == name) return row.supportsToolCalls; return std::nullopt; }
};

class CapturingProvider final : public provider::IChatProvider
{
  public:
    enum class Mode {
        PlainText,
        BridgeToolCall,
        ActionV3ToolCall,
        InvalidBridgeThenValidActionV3,
        NativeEmptyArguments,
        SemanticFailure,
    };
    explicit CapturingProvider(Mode mode = Mode::PlainText) : mode_(mode) {}

    platform::Result<provider::ProviderResponse> generate(
        const provider::ProviderRequest& request,
        provider::ProviderCallContext&) override
    {
        captured = request;
        capturedRequests.push_back(request);
        ++calls;
        if (mode_ == Mode::BridgeToolCall)
        {
            provider::ProviderResponse response;
            response.text = request.input +
                "\n<function_calls><function_call><tool>read_file</tool>"
                "<args_json><![CDATA[{\"path\":\"synthetic.txt\"}]]></args_json>"
                "</function_call></function_calls>";
            return platform::Result<provider::ProviderResponse>::success(
                std::move(response));
        }
        if (mode_ == Mode::ActionV3ToolCall)
        {
            const auto begin = request.input.find("<Function_");
            const auto end = begin == std::string::npos
                ? std::string::npos
                : request.input.find("/>", begin);
            if (begin == std::string::npos || end == std::string::npos) {
                return platform::Result<provider::ProviderResponse>::failure(
                    platform::Error::providerError("missing bridge sentinel"));
            }

            provider::ProviderResponse response;
            response.text = request.input.substr(begin, end - begin + 2) +
                "\n{\"protocol\":\"action-v3\",\"tool_calls\":[{\"name\":\"read_file\","
                "\"arguments\":{\"path\":\"synthetic.txt\"}}]}";
            return platform::Result<provider::ProviderResponse>::success(
                std::move(response));
        }
        if (mode_ == Mode::InvalidBridgeThenValidActionV3)
        {
            if (bridgeTrigger.empty()) {
                const auto begin = request.input.find("<Function_");
                const auto end = begin == std::string::npos
                    ? std::string::npos
                    : request.input.find("/>", begin);
                if (begin != std::string::npos && end != std::string::npos) {
                    bridgeTrigger = request.input.substr(begin, end - begin + 2);
                }
            }

            provider::ProviderResponse response;
            if (calls == 1) {
                // Same sentinel, but an invalid action-v3 body.  This makes
                // the correction include a concrete parser error rather than
                // merely reporting a missing trigger marker.
                response.text = bridgeTrigger +
                    "\n{\"protocol\":\"action-v3\",\"tool_calls\":[{\"name\":\"read_file\",\"arguments\":{\"path\":\"synthetic.txt\"}}]}]}";
            } else {
                response.text = bridgeTrigger +
                    "\n{\"protocol\":\"action-v3\",\"tool_calls\":[{\"name\":\"read_file\",\"arguments\":{\"path\":\"synthetic.txt\"}}]}";
            }
            return platform::Result<provider::ProviderResponse>::success(
                std::move(response));
        }
        if (mode_ == Mode::NativeEmptyArguments)
        {
            provider::ProviderResponse response;
            provider::ToolCall call;
            call.id = "call-synthetic";
            call.name = "ping";
            call.arguments = "";
            response.toolCalls.push_back(std::move(call));
            return platform::Result<provider::ProviderResponse>::success(
                std::move(response));
        }
        if (mode_ == Mode::SemanticFailure)
        {
            return platform::Result<provider::ProviderResponse>::failure(
                platform::Error(platform::ErrorCode::RateLimited,
                                "synthetic quota exhausted",
                                "fixture diagnostic",
                                "fixture-rate-limit",
                                429));
        }
        provider::ProviderResponse response;
        response.text = "synthetic bridge answer";
        return platform::Result<provider::ProviderResponse>::success(std::move(response));
    }

    provider::ProviderCapabilities capabilities() const noexcept override
    {
        return provider::ProviderCapabilities{};
    }

    provider::ProviderRequest captured;
    std::vector<provider::ProviderRequest> capturedRequests;
    int calls = 0;
  private:
    Mode mode_;
    std::string bridgeTrigger;
};

class CollectingSink final : public IResponseSink
{
  public:
    void onEvent(const generation::GenerationEvent& event) override { events.push_back(event); }
    void onClose() override { closed = true; }
    bool isValid() const override { return !closed; }
    std::string getSinkType() const override { return "generation-bridge-fixture"; }
    std::vector<generation::GenerationEvent> events;
    bool closed = false;
};

std::string firstToolCallId(const CollectingSink& sink)
{
    const auto event = std::find_if(
        sink.events.begin(), sink.events.end(), [](const auto& value) {
            return std::holds_alternative<generation::ToolCallDone>(value);
        });
    if (event == sink.events.end()) return "";
    return std::get<generation::ToolCallDone>(*event).id;
}

CurrentInputPart currentTextPart(std::string text)
{
    CurrentInputPart part;
    part.text = std::move(text);
    return part;
}

CurrentInputPart currentToolResultPart(std::string id, std::string text)
{
    CurrentInputPart part;
    part.text = std::move(text);
    part.toolResultCallId = std::move(id);
    part.isToolResult = true;
    return part;
}

void setCurrentInput(GenerationRequest& request,
                     std::vector<CurrentInputPart> parts)
{
    request.currentInput.clear();
    for (const auto& part : parts) request.currentInput += part.text;
    request.currentInputParts = std::move(parts);
    request.messages = {Message::user(request.currentInput)};
}

std::vector<ToolDefinition> fixtureTools()
{
    ToolDefinition tool;
    tool.name = "read_file";
    tool.originalName = tool.name;
    tool.description = "read a synthetic file";
    tool.inputSchema["type"] = "object";
    tool.inputSchema["properties"]["path"]["type"] = "string";
    tool.inputSchema["required"].append("path");
    return {std::move(tool)};
}

std::vector<ToolDefinition> pingTool()
{
    ToolDefinition tool;
    tool.name = "ping";
    tool.originalName = tool.name;
    tool.inputSchema["type"] = "object";
    tool.inputSchema["properties"] = Json::Value(Json::objectValue);
    return {std::move(tool)};
}

FakeChannelCatalog makeChannel(const std::string& name, bool supportsTools)
{
    FakeChannelCatalog channels;
    Channelinfo_st channel;
    channel.channelName = name;
    channel.channelStatus = true;
    channel.supportsToolCalls = supportsTools;
    channels.rows.push_back(channel);
    return channels;
}

GenerationRequest makeRequest(const std::string& provider,
                              std::vector<ToolDefinition> tools)
{
    GenerationRequest request;
    request.provider = provider;
    request.model = "fixture-model";
    request.currentInput = "synthetic bridge question";
    request.messages = {Message::user(request.currentInput)};
    request.toolDefinitions = std::move(tools);
    request.toolChoiceSpec.mode = ToolChoiceMode::Auto;
    request.clientInfo["client_type"] = "generic-fixture-client";
    request.requestId = "bridge-fixture-request";
    request.continuityTexts = {request.currentInput};
    return request;
}

}  // namespace

DROGON_TEST(SessionCodec_RoundTripsToolResultLedger)
{
    session_st source;
    source.provider.pendingToolCallIds = {"call_pending_a", "call_pending_b"};
    source.provider.consumedToolResultIds = {"call_consumed_a", "call_consumed_b"};

    const Json::Value encoded = sessioncodec::encodeSession(source);
    CHECK(encoded["v"].asInt() == 2);
    const session_st decoded = sessioncodec::decodeSession(encoded);
    CHECK(decoded.provider.pendingToolCallIds == source.provider.pendingToolCallIds);
    CHECK(decoded.provider.consumedToolResultIds == source.provider.consumedToolResultIds);
}

DROGON_TEST(GenerationService_ToolBridgeTransformsRequestThroughRunGuarded)
{
    auto channels = makeChannel("bridge-fixture", false);

    auto provider = std::make_shared<CapturingProvider>();
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("bridge-fixture", provider));

    auto request = makeRequest("bridge-fixture", fixtureTools());

    CollectingSink sink;
    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
    chatSession sessionStore;
    GenerationService service(&registry, &sessionStore, &responseIndex, &executionGate, &channels);
    const auto error = service.runGuarded(request, sink);

    CHECK(!error.has_value());
    CHECK(provider->calls == 1);
    CHECK(provider->captured.input.find("<tool_instructions>") != std::string::npos);
    CHECK(provider->captured.input.find("read_file") != std::string::npos);
    // The provider receives the normalized value DTO rather than the legacy
    // session bag; tool definitions are still available for native providers.
    CHECK(provider->captured.tools.size() == 1);
    CHECK(provider->captured.tools[0].name == "read_file");
    CHECK(sink.closed);
}

DROGON_TEST(GenerationService_RuntimeConfigReachesToolBridgePipeline)
{
    auto channels = makeChannel("bridge-config-fixture", false);
    auto provider = std::make_shared<CapturingProvider>();
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("bridge-config-fixture", provider));

    Json::Value runtimeConfig(Json::objectValue);
    runtimeConfig["tool_bridge"]["format_by_channel"]["bridge-config-fixture"] = "json";
    runtimeConfig["tool_bridge"]["allow_format_fallback"] = true;

    auto request = makeRequest("bridge-config-fixture", fixtureTools());
    CollectingSink sink;
    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
    chatSession sessionStore;
    GenerationService service(&registry, &sessionStore, &responseIndex, &executionGate,
                              &channels, runtimeConfig);

    CHECK(!service.runGuarded(request, sink).has_value());
    CHECK(provider->calls == 1);
    // The generic fixture client defaults to XML.  Seeing the action-v3 JSON
    // policy proves the AppWiring-provided value crossed Service -> Pipeline
    // -> ToolDefinitionEncoder rather than falling back to global runtime state.
    CHECK(provider->captured.input.find("action-v3") != std::string::npos);
    CHECK(provider->captured.input.find("<function_calls>") == std::string::npos);
    CHECK(sink.closed);
}

DROGON_TEST(GenerationService_BridgeCodecAndEmitOrderRunThroughProductionPipeline)
{
    auto channels = makeChannel("bridge-emit-fixture", false);
    auto provider = std::make_shared<CapturingProvider>(CapturingProvider::Mode::BridgeToolCall);
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("bridge-emit-fixture", provider));
    auto request = makeRequest("bridge-emit-fixture", fixtureTools());
    CollectingSink sink;
    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
    chatSession sessionStore;
    GenerationService service(&registry, &sessionStore, &responseIndex, &executionGate, &channels);
    CHECK(!service.runGuarded(request, sink).has_value());

    REQUIRE(sink.events.size() >= 3);
    CHECK(std::holds_alternative<generation::Started>(sink.events.front()));
    const auto toolEvent = std::find_if(
        sink.events.begin(), sink.events.end(), [](const auto& event) {
            return std::holds_alternative<generation::ToolCallDone>(event);
        });
    REQUIRE(toolEvent != sink.events.end());
    const auto textEvent = std::find_if(
        sink.events.begin(), sink.events.end(), [](const auto& event) {
            return std::holds_alternative<generation::OutputTextDone>(event);
        });
    if (textEvent != sink.events.end()) CHECK(toolEvent < textEvent);
    const auto call = std::get<generation::ToolCallDone>(*toolEvent);
    CHECK(call.name == "read_file");
    CHECK(call.arguments.find("synthetic.txt") != std::string::npos);
    CHECK(std::holds_alternative<generation::Completed>(sink.events.back()));
    CHECK(std::get<generation::Completed>(sink.events.back()).finishReason == "tool_calls");
    CHECK(sink.closed);
}

DROGON_TEST(GenerationService_AssignsUniqueIdsToSequentialBridgeCalls)
{
    auto channels = makeChannel("bridge-id-fixture", false);
    auto provider = std::make_shared<CapturingProvider>(
        CapturingProvider::Mode::ActionV3ToolCall);
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("bridge-id-fixture", provider));

    Json::Value runtimeConfig(Json::objectValue);
    runtimeConfig["tool_bridge"]["format_by_channel"]["bridge-id-fixture"] = "json";

    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
    chatSession sessionStore;
    GenerationService service(&registry, &sessionStore, &responseIndex, &executionGate,
                              &channels, runtimeConfig);

    auto firstRequest = makeRequest("bridge-id-fixture", fixtureTools());
    firstRequest.clientInfo["client_type"] = "ClaudeCode";
    firstRequest.requestId = "bridge-id-fixture-first";
    CollectingSink firstSink;
    CHECK(!service.runGuarded(firstRequest, firstSink).has_value());

    auto secondRequest = makeRequest("bridge-id-fixture", fixtureTools());
    secondRequest.clientInfo["client_type"] = "ClaudeCode";
    secondRequest.requestId = "bridge-id-fixture-second";
    CollectingSink secondSink;
    CHECK(!service.runGuarded(secondRequest, secondSink).has_value());

    const std::string firstId = firstToolCallId(firstSink);
    const std::string secondId = firstToolCallId(secondSink);
    CHECK(firstId.rfind("call_", 0) == 0);
    CHECK(secondId.rfind("call_", 0) == 0);
    CHECK(firstId != secondId);
}

DROGON_TEST(GenerationService_SuppressesReplayedToolResultsAfterSessionResolution)
{
    auto channels = makeChannel("tool-result-ledger-fixture", false);
    auto provider = std::make_shared<CapturingProvider>(
        CapturingProvider::Mode::ActionV3ToolCall);
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("tool-result-ledger-fixture", provider));

    Json::Value runtimeConfig(Json::objectValue);
    runtimeConfig["tool_bridge"]["format_by_channel"]["tool-result-ledger-fixture"] = "json";

    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
    chatSession sessionStore;
    sessionStore.setTrackingMode(SessionTrackingMode::ZeroWidth);
    GenerationService service(&registry, &sessionStore, &responseIndex, &executionGate,
                              &channels, runtimeConfig);

    const std::string stableSessionId = "sess_tool_result_ledger";
    const std::string continuityMarker = ZeroWidthEncoder::appendEncoded(
        "continuity", stableSessionId);

    auto firstRequest = makeRequest("tool-result-ledger-fixture", fixtureTools());
    firstRequest.requestId = "tool-result-ledger-first";
    firstRequest.continuityTexts = {continuityMarker};
    setCurrentInput(firstRequest, {currentTextPart("initial turn")});
    CollectingSink firstSink;
    CHECK(!service.runGuarded(firstRequest, firstSink).has_value());
    const std::string firstCallId = firstToolCallId(firstSink);
    REQUIRE(!firstCallId.empty());

    const std::string firstResult = "result-from-first-call-unique";
    auto secondRequest = makeRequest("tool-result-ledger-fixture", fixtureTools());
    secondRequest.requestId = "tool-result-ledger-second";
    secondRequest.continuityTexts = {continuityMarker};
    setCurrentInput(secondRequest, {
        currentTextPart("second turn\n"),
        currentToolResultPart(firstCallId,
            "[tool_result tool_use_id=" + firstCallId + "]\n" + firstResult +
            "\n[/tool_result]\n"),
    });
    CollectingSink secondSink;
    CHECK(!service.runGuarded(secondRequest, secondSink).has_value());
    REQUIRE(provider->capturedRequests.size() == 2);
    CHECK(provider->capturedRequests[1].input.find(firstResult) != std::string::npos);
    const std::string secondCallId = firstToolCallId(secondSink);
    REQUIRE(!secondCallId.empty());
    CHECK(secondCallId != firstCallId);

    const std::string secondResult = "result-from-second-call-unique";
    auto thirdRequest = makeRequest("tool-result-ledger-fixture", fixtureTools());
    thirdRequest.requestId = "tool-result-ledger-third";
    thirdRequest.continuityTexts = {continuityMarker};
    setCurrentInput(thirdRequest, {
        currentTextPart("third turn\n"),
        currentToolResultPart(firstCallId,
            "[tool_result tool_use_id=" + firstCallId + "]\n" + firstResult +
            "\n[/tool_result]\n"),
        currentToolResultPart(secondCallId,
            "[tool_result tool_use_id=" + secondCallId + "]\n" + secondResult +
            "\n[/tool_result]\n"),
    });
    CollectingSink thirdSink;
    CHECK(!service.runGuarded(thirdRequest, thirdSink).has_value());
    REQUIRE(provider->capturedRequests.size() == 3);
    const auto& thirdProviderInput = provider->capturedRequests[2].input;
    CHECK(thirdProviderInput.find("third turn") != std::string::npos);
    CHECK(thirdProviderInput.find(secondResult) != std::string::npos);
    CHECK(thirdProviderInput.find(firstResult) == std::string::npos);

    session_st persisted;
    sessionStore.getSession(stableSessionId, persisted);
    CHECK(std::find(persisted.provider.consumedToolResultIds.begin(),
                    persisted.provider.consumedToolResultIds.end(), firstCallId) !=
          persisted.provider.consumedToolResultIds.end());
    CHECK(std::find(persisted.provider.consumedToolResultIds.begin(),
                    persisted.provider.consumedToolResultIds.end(), secondCallId) !=
          persisted.provider.consumedToolResultIds.end());
}

DROGON_TEST(GenerationService_CodexBridgeRetryReusesCurrentThreadAndSendsCorrection)
{
    auto channels = makeChannel("codex-bridge-retry-fixture", false);
    auto provider = std::make_shared<CapturingProvider>(
        CapturingProvider::Mode::InvalidBridgeThenValidActionV3);
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("codex-bridge-retry-fixture", provider));

    auto request = makeRequest("codex-bridge-retry-fixture", fixtureTools());
    request.clientInfo["client_type"] = "codex";
    CollectingSink sink;
    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
    chatSession sessionStore;
    GenerationService service(&registry, &sessionStore, &responseIndex, &executionGate, &channels);

    CHECK(!service.runGuarded(request, sink).has_value());
    REQUIRE(provider->calls == 2);
    REQUIRE(provider->capturedRequests.size() == 2);
    const auto& first = provider->capturedRequests[0];
    const auto& correction = provider->capturedRequests[1];

    CHECK(first.previousConversationId.empty());
    CHECK(!first.conversationId.empty());
    CHECK(correction.conversationId == first.conversationId);
    CHECK(correction.previousConversationId == first.conversationId);
    CHECK(correction.previousConversationFallbackId.empty());
    CHECK(correction.input.find("上一条回复格式不正确") != std::string::npos);
    CHECK(correction.input.find("action-v3 response must be one valid JSON object") !=
          std::string::npos);
    CHECK(correction.input.find("synthetic bridge question") == std::string::npos);
    CHECK(correction.input.find("<tool_instructions>") == std::string::npos);
    CHECK(correction.rawInput == correction.input);

    const auto toolEvent = std::find_if(
        sink.events.begin(), sink.events.end(), [](const auto& event) {
            return std::holds_alternative<generation::ToolCallDone>(event);
        });
    REQUIRE(toolEvent != sink.events.end());
    CHECK(std::get<generation::ToolCallDone>(*toolEvent).name == "read_file");
    CHECK(sink.closed);
}

DROGON_TEST(GenerationService_NativeToolArgumentsAreNormalizedBeforeEmit)
{
    auto channels = makeChannel("native-emit-fixture", true);
    auto provider = std::make_shared<CapturingProvider>(CapturingProvider::Mode::NativeEmptyArguments);
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("native-emit-fixture", provider));
    auto request = makeRequest("native-emit-fixture", pingTool());
    CollectingSink sink;
    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
    chatSession sessionStore;
    GenerationService service(&registry, &sessionStore, &responseIndex, &executionGate, &channels);
    CHECK(!service.runGuarded(request, sink).has_value());

    REQUIRE(sink.events.size() == 3);
    REQUIRE(std::holds_alternative<generation::ToolCallDone>(sink.events[1]));
    const auto call = std::get<generation::ToolCallDone>(sink.events[1]);
    CHECK(call.name == "ping");
    CHECK(call.arguments == "{}");
    CHECK(std::holds_alternative<generation::Completed>(sink.events[2]));
}

DROGON_TEST(GenerationService_RequiredToolFallbackRunsInsideEmitResultEvents)
{
    auto channels = makeChannel("forced-tool-fixture", false);
    auto provider = std::make_shared<CapturingProvider>();
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("forced-tool-fixture", provider));
    auto request = makeRequest("forced-tool-fixture", pingTool());
    request.toolChoiceSpec.mode = ToolChoiceMode::Specific;
    request.toolChoiceSpec.toolName = "ping";
    CollectingSink sink;
    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
    chatSession sessionStore;
    GenerationService service(&registry, &sessionStore, &responseIndex, &executionGate, &channels);
    CHECK(!service.runGuarded(request, sink).has_value());

    bool foundPing = false;
    for (const auto& event : sink.events)
    {
        if (std::holds_alternative<generation::ToolCallDone>(event) &&
            std::get<generation::ToolCallDone>(event).name == "ping")
            foundPing = true;
    }
    CHECK(foundPing);
    CHECK(sink.closed);
}

DROGON_TEST(GenerationService_ProviderFailurePreservesSemanticErrorAndCloses)
{
    auto channels = makeChannel("failure-fixture", true);
    auto provider = std::make_shared<CapturingProvider>(CapturingProvider::Mode::SemanticFailure);
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("failure-fixture", provider));
    auto request = makeRequest("failure-fixture", {});
    CollectingSink sink;
    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
    chatSession sessionStore;
    GenerationService service(&registry, &sessionStore, &responseIndex, &executionGate, &channels);

    CHECK(!service.runGuarded(request, sink).has_value());
    CHECK(provider->calls == 1);
    REQUIRE(sink.events.size() == 2);
    CHECK(std::holds_alternative<generation::Started>(sink.events[0]));
    REQUIRE(std::holds_alternative<generation::Error>(sink.events[1]));
    const auto& error = std::get<generation::Error>(sink.events[1]);
    CHECK(error.code == platform::ErrorCode::RateLimited);
    CHECK(error.message == "synthetic quota exhausted");
    CHECK(error.providerCode == "fixture-rate-limit");
    CHECK(error.detail == "fixture diagnostic");
    CHECK(sink.closed);
}
