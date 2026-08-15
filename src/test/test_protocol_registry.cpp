#include <drogon/drogon_test.h>

#include <application/generation/protocol/common/ProtocolRegistry.h>
#include <application/generation/protocol/openai/OpenAiProtocolModule.h>

#include <stdexcept>
#include <utility>

using namespace generation::protocol;

namespace {

class SimulatedRequestAdapter final : public IProtocolRequestAdapter
{
  public:
    AdapterResult adapt(const RawProtocolRequest& raw) const override
    {
        GenerationRequest request;
        request.model = raw.body.get("model", "sim-model").asString();
        request.currentInput = raw.body.get("prompt", "").asString();
        return AdapterResult{std::move(request), {}};
    }
};

class SimulatedSinkFactory final : public IProtocolResponseSinkFactory
{
  public:
    std::shared_ptr<IResponseSink> create(const ResponseContext& context) const override
    {
        lastOperation = context.operation;
        return std::make_shared<NullSink>();
    }

    mutable std::string lastOperation;
};

class SimulatedCapabilityMapper final : public ICapabilityMapper
{
  public:
    GenerationCapabilities capabilities(const std::string&) const override
    {
        return GenerationCapabilities::all();
    }
};

class SimulatedProtocolModule final : public IProtocolModule
{
  public:
    std::string id() const override { return "simulated"; }
    std::string version() const override { return "v1"; }
    std::vector<std::string> operations() const override { return {"generate"}; }
    const IProtocolRequestAdapter& requestAdapter(const std::string& operation) const override
    {
        if (operation != "generate") throw std::out_of_range("operation");
        return adapter_;
    }
    const IProtocolResponseSinkFactory& responseSinkFactory(
        const std::string& operation) const override
    {
        (void)requestAdapter(operation);
        return sinkFactory_;
    }
    const ICapabilityMapper& capabilityMapper() const override { return capabilities_; }
    const std::string& lastSinkOperation() const { return sinkFactory_.lastOperation; }

  private:
    SimulatedRequestAdapter adapter_;
    SimulatedSinkFactory sinkFactory_;
    SimulatedCapabilityMapper capabilities_;
};

}  // namespace

DROGON_TEST(ProtocolRegistry_DefaultRoutesAndOperations)
{
    const auto registry = makeDefaultProtocolRegistry();
    REQUIRE(registry);

    std::string error;
    CHECK(registry->validate(&error));
    CHECK(error.empty());

    const auto* chat = registry->findRoute("post", "/chaynsapi/v1/chat/completions");
    REQUIRE(chat);
    CHECK(chat->id() == "openai-compatible");
    CHECK(registry->findRouteOperation("POST", "/chaynsapi/v1/chat/completions") ==
          "chat.completions");
    CHECK(registry->findRouteOperation("POST", "/retoolapi/v1/responses") ==
          "responses.create");
}

DROGON_TEST(ProtocolRegistry_RejectsDuplicateModulesAndRoutes)
{
    ProtocolRegistry registry;
    auto first = generation::protocol::openai::makeOpenAiProtocolModule();
    REQUIRE(registry.registerModule(first));
    CHECK(!registry.registerModule(generation::protocol::openai::makeOpenAiProtocolModule()));
    CHECK(registry.lastError().find("duplicate") != std::string::npos);

    CHECK(registry.registerRoute("POST", "/v1/test", "openai-compatible",
                                 "chat.completions"));
    CHECK(!registry.registerRoute("POST", "/v1/test", "openai-compatible",
                                  "chat.completions"));
}

DROGON_TEST(OpenAiProtocolAdapter_PopulatesUnifiedBoundary)
{
    auto module = generation::protocol::openai::makeOpenAiProtocolModule();
    Json::Value body;
    body["model"] = "GPT-4o";
    body["messages"] = Json::Value(Json::arrayValue);
    Json::Value message;
    message["role"] = "user";
    message["content"] = "hello";
    body["messages"].append(message);
    body["metadata"]["trace_tag"] = "safe";
    body["metadata"]["api_key"] = "must_not_cross_boundary";

    RawProtocolRequest raw;
    raw.method = "POST";
    raw.path = "/chaynsapi/v1/chat/completions";
    raw.body = body;

    const auto result = module->requestAdapter("chat.completions").adapt(raw);
    REQUIRE(result.succeeded());
    CHECK(result.request->responseLifecycle == ResponseLifecycle::Immediate);
    CHECK(result.request->currentInput.find("hello") != std::string::npos);
    CHECK(result.request->protocolExtensions["metadata"]["trace_tag"].asString() == "safe");
    CHECK(!result.request->protocolExtensions["metadata"].isMember("api_key"));
}

DROGON_TEST(OpenAiProtocolModule_ExposesCapabilitiesAndOwnsSinkFactory)
{
    auto module = generation::protocol::openai::makeOpenAiProtocolModule();

    const auto capabilities = module->capabilityMapper().capabilities("responses.create");
    CHECK(capabilities.text);
    CHECK(capabilities.streaming);
    CHECK(capabilities.tools);

    ResponseContext context;
    context.operation = "responses.create";
    context.model = "sim-model";
    context.jsonResponse = [](const Json::Value&, int) {};
    auto sink = module->responseSinkFactory("responses.create").create(context);
    REQUIRE(sink);
    CHECK(sink->getSinkType() == "OpenAiResponsesJsonSink");
}

DROGON_TEST(ProtocolRegistry_SimulatedProtocolNeedsOnlyBoundaryChanges)
{
    ProtocolRegistry registry;
    auto module = std::make_shared<SimulatedProtocolModule>();
    REQUIRE(registry.registerModule(module));
    REQUIRE(registry.registerRoute("POST", "/simulated/v1/generate", "simulated", "generate"));

    RawProtocolRequest raw;
    raw.method = "POST";
    raw.path = "/simulated/v1/generate";
    raw.body["model"] = "sim-model";
    raw.body["prompt"] = "boundary only";
    const auto dispatch = registry.dispatch(raw);

    REQUIRE(dispatch.succeeded());
    CHECK(dispatch.module->id() == "simulated");
    CHECK(dispatch.operation == "generate");
    CHECK(dispatch.adaptation.request->currentInput == "boundary only");
    REQUIRE(dispatch.responseSinkFactory);
    ResponseContext context;
    context.operation = dispatch.operation;
    CHECK(dispatch.responseSinkFactory->create(context));
    CHECK(module->lastSinkOperation() == "generate");
}

DROGON_TEST(ProtocolRegistry_UnknownRouteHasNoProtocolFallback)
{
    auto registry = makeDefaultProtocolRegistry();
    RawProtocolRequest raw;
    raw.method = "POST";
    raw.path = "/unregistered/v1/chat/completions";
    raw.body["model"] = "GPT-4o";

    const auto dispatch = registry->dispatch(raw);
    CHECK(!dispatch.succeeded());
    CHECK(dispatch.module == nullptr);
    REQUIRE(dispatch.adaptation.error.has_value());
    CHECK(dispatch.adaptation.error->message == "Unknown protocol route");
}

DROGON_TEST(GenerationCapabilities_IntersectionIsExplicit)
{
    GenerationCapabilities protocol = GenerationCapabilities::all();
    GenerationCapabilities model = GenerationCapabilities::all();
    GenerationCapabilities provider = GenerationCapabilities::all();
    provider.images = false;
    provider.parallelTools = false;

    const auto effective = intersectCapabilities(protocol, model, provider);
    CHECK(!effective.images);
    CHECK(!effective.parallelTools);
    CHECK(effective.tools);
}
