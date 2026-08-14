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

class CapturingProvider final : public APIinterface
{
  public:
    enum class Mode { PlainText, BridgeToolCall, NativeEmptyArguments };
    explicit CapturingProvider(Mode mode = Mode::PlainText) : mode_(mode) {}

    provider::ProviderResult generate(session_st& session) override
    {
        captured = session;
        ++calls;
        if (mode_ == Mode::BridgeToolCall)
        {
            return provider::ProviderResult::success(
                session.provider.toolBridgeTrigger +
                "\n<function_calls><function_call><tool>read_file</tool>"
                "<args_json><![CDATA[{\"path\":\"synthetic.txt\"}]]></args_json>"
                "</function_call></function_calls>");
        }
        if (mode_ == Mode::NativeEmptyArguments)
        {
            auto result = provider::ProviderResult::success("");
            provider::ToolCall call;
            call.id = "call-synthetic";
            call.name = "ping";
            call.arguments = "";
            result.toolCalls.push_back(call);
            return result;
        }
        return provider::ProviderResult::success("synthetic bridge answer");
    }
    void checkAlivableTokens() override {}
    void checkModels() override {}
    ProviderModelCatalog getModels() override { return {}; }
    void init() override {}
    void afterResponseProcess(session_st&) override {}
    void eraseChatinfoMap(std::string) override {}
    void transferThreadContext(const std::string&, const std::string&) override {}

    session_st captured;
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
    REQUIRE(registry.registerProvider("bridge-fixture", provider));

    auto request = makeRequest("bridge-fixture", fixtureTools());

    CollectingSink sink;
    ResponseIndex responseIndex;
    session::SessionExecutionGate executionGate;
    chatSession sessionStore;
    GenerationService service(&registry, &sessionStore, &responseIndex, &executionGate, &channels);
    const auto error = service.runGuarded(request, sink);

    CHECK(!error.has_value());
    CHECK(provider->calls == 1);
    CHECK(provider->captured.request.message.find("<tool_instructions>") != std::string::npos);
    CHECK(provider->captured.request.message.find("read_file") != std::string::npos);
    // The bridge consumes the structured list before the provider call; the
    // raw snapshot is retained for response diagnostics and codec selection.
    CHECK(provider->captured.request.tools.isNull());
    CHECK(provider->captured.request.toolsRaw.isArray());
    CHECK(provider->captured.provider.toolBridgeTrigger.empty() == false);
    CHECK(provider->captured.provider.toolBridgeFormat != toolcall::BridgeWireFormat::Unset);
    CHECK(sink.closed);
}

DROGON_TEST(GenerationService_BridgeCodecAndEmitOrderRunThroughProductionPipeline)
{
    auto channels = makeChannel("bridge-emit-fixture", false);
    auto provider = std::make_shared<CapturingProvider>(CapturingProvider::Mode::BridgeToolCall);
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerProvider("bridge-emit-fixture", provider));
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
    REQUIRE(registry.registerProvider("native-emit-fixture", provider));
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
    REQUIRE(registry.registerProvider("forced-tool-fixture", provider));
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
