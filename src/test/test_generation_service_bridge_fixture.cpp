#include <drogon/drogon_test.h>

#include <domain/port/IChannelCatalog.h>
#include <infrastructure/provider/ProviderRegistry.h>
#include <sessionManager/continuity/ResponseIndex.h>
#include <sessionManager/core/SessionExecutionGate.h>
#include <sessionManager/core/Session.h>
#include <sessionManager/contracts/IResponseSink.h>
#include <sessionManager/core/GenerationService.h>

#include <algorithm>
#include <list>
#include <memory>
#include <string>

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
        NativeEmptyArguments,
        SemanticFailure,
    };
    explicit CapturingProvider(Mode mode = Mode::PlainText) : mode_(mode) {}

    platform::Result<provider::ProviderResponse> generate(
        const provider::ProviderRequest& request,
        provider::ProviderCallContext&) override
    {
        captured = request;
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
    int calls = 0;
  private:
    Mode mode_;
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

Json::Value fixtureTools()
{
    Json::Value tools(Json::arrayValue);
    Json::Value tool(Json::objectValue);
    tool["type"] = "function";
    tool["function"]["name"] = "read_file";
    tool["function"]["description"] = "read a synthetic file";
    tool["function"]["parameters"]["type"] = "object";
    tool["function"]["parameters"]["properties"]["path"]["type"] = "string";
    tool["function"]["parameters"]["required"].append("path");
    tools.append(tool);
    return tools;
}

Json::Value pingTool()
{
    Json::Value tools(Json::arrayValue);
    Json::Value tool(Json::objectValue);
    tool["type"] = "function";
    tool["function"]["name"] = "ping";
    tool["function"]["parameters"]["type"] = "object";
    tool["function"]["parameters"]["properties"] = Json::Value(Json::objectValue);
    tools.append(tool);
    return tools;
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

GenerationRequest makeRequest(const std::string& provider, Json::Value tools)
{
    GenerationRequest request;
    request.provider = provider;
    request.model = "fixture-model";
    request.currentInput = "synthetic bridge question";
    request.messages = {Message::user(request.currentInput)};
    request.tools = std::move(tools);
    request.toolsRaw = request.tools;
    request.toolChoice = "auto";
    request.clientInfo["client_type"] = "generic-fixture-client";
    request.requestId = "bridge-fixture-request";
    request.continuityTexts = {request.currentInput};
    return request;
}

}  // namespace

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
    request.toolChoice = R"({"type":"function","function":{"name":"ping"}})";
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
    auto request = makeRequest("failure-fixture", Json::Value(Json::arrayValue));
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
