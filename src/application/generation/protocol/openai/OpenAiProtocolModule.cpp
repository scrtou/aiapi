#include <application/generation/protocol/openai/OpenAiProtocolModule.h>

#include <application/generation/core/RequestAdapters.h>

#include <stdexcept>
#include <cctype>
#include <utility>

namespace generation::protocol::openai {
namespace {

class RequestAdapter final : public IProtocolRequestAdapter
{
  public:
    explicit RequestAdapter(bool responses) : responses_(responses) {}

    AdapterResult adapt(const RawProtocolRequest& raw) const override
    {
        if (!raw.body.isObject()) {
            return AdapterResult{{}, platform::Error::badRequest("Request body must be a JSON object")};
        }
        GenerationRequest request = responses_
            ? RequestAdapters::buildGenerationRequestFromResponses(raw.body, raw.headers)
            : RequestAdapters::buildGenerationRequestFromChat(raw.body, raw.headers);
        // The provider is selected by the route/use case, never by the
        // protocol adapter.  Raw body extensions remain boundary-local.
        if (!request.protocolExtensions.isObject()) {
            request.protocolExtensions = Json::Value(Json::objectValue);
        }
        if (raw.body.isMember("metadata") && raw.body["metadata"].isObject()) {
            for (const auto& name : raw.body["metadata"].getMemberNames()) {
                std::string lowered = name;
                for (auto& value : lowered) {
                    value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
                }
                if (lowered.find("token") != std::string::npos ||
                    lowered.find("secret") != std::string::npos ||
                    lowered.find("password") != std::string::npos ||
                    lowered.find("authorization") != std::string::npos ||
                    lowered.find("api_key") != std::string::npos) {
                    continue;
                }
                request.protocolExtensions["metadata"][name] = raw.body["metadata"][name];
            }
        }
        return AdapterResult{std::move(request), {}};
    }

  private:
    bool responses_;
};

class SinkFactory final : public IProtocolResponseSinkFactory
{
  public:
    explicit SinkFactory(SinkFactoryCallback callback) : callback_(std::move(callback)) {}

    std::shared_ptr<IResponseSink> create(const ResponseContext& context) const override
    {
        if (callback_) return callback_(context);
        // Transport wiring is intentionally injected by the edge.  Never
        // silently discard a response when the edge forgot to bind a sink.
        return nullptr;
    }

  private:
    SinkFactoryCallback callback_;
};

class CapabilityMapper final : public ICapabilityMapper
{
  public:
    GenerationCapabilities capabilities(const std::string&) const override
    {
        GenerationCapabilities result;
        result.text = true;
        result.images = true;
        result.streaming = true;
        result.tools = true;
        result.parallelTools = true;
        result.reasoning = true;
        result.continuity = true;
        return result;
    }
};

}  // namespace

OpenAiProtocolModule::OpenAiProtocolModule(SinkFactoryCallback sinkFactory)
    : chatAdapter_(std::make_unique<RequestAdapter>(false)),
      responsesAdapter_(std::make_unique<RequestAdapter>(true)),
      sinkFactory_(std::make_unique<SinkFactory>(std::move(sinkFactory))),
      capabilityMapper_(std::make_unique<CapabilityMapper>())
{
}

std::string OpenAiProtocolModule::id() const { return "openai-compatible"; }
std::string OpenAiProtocolModule::version() const { return "v1"; }

std::vector<std::string> OpenAiProtocolModule::operations() const
{
    return {"chat.completions", "responses.create"};
}

const IProtocolRequestAdapter& OpenAiProtocolModule::requestAdapter(
    const std::string& operation) const
{
    if (operation == "chat.completions") return *chatAdapter_;
    if (operation == "responses.create") return *responsesAdapter_;
    throw std::out_of_range("unknown OpenAI operation: " + operation);
}

const IProtocolResponseSinkFactory& OpenAiProtocolModule::responseSinkFactory(
    const std::string& operation) const
{
    (void)requestAdapter(operation);
    return *sinkFactory_;
}

const ICapabilityMapper& OpenAiProtocolModule::capabilityMapper() const
{
    return *capabilityMapper_;
}

std::shared_ptr<OpenAiProtocolModule> makeOpenAiProtocolModule(SinkFactoryCallback sinkFactory)
{
    return std::make_shared<OpenAiProtocolModule>(std::move(sinkFactory));
}

}  // namespace generation::protocol::openai
