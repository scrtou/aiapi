#include <application/generation/protocol/openai/OpenAiProtocolModule.h>
#include <application/generation/protocol/openai/OpenAiChatJsonSink.h>
#include <application/generation/protocol/openai/OpenAiChatSseSink.h>
#include <application/generation/protocol/openai/OpenAiRequestAdapter.h>
#include <application/generation/protocol/openai/OpenAiResponsesJsonSink.h>
#include <application/generation/protocol/openai/OpenAiResponsesSseSink.h>

#include <stdexcept>
#include <utility>

namespace generation::protocol::openai {
namespace {

class SinkFactory final : public IProtocolResponseSinkFactory
{
  public:
    std::shared_ptr<IResponseSink> create(const ResponseContext& context) const override
    {
        if (context.operation == "chat.completions") {
            if (context.stream) {
                if (!context.streamWriter || !context.close) return nullptr;
                return std::make_shared<OpenAiChatSseSink>(
                    context.streamWriter, context.close, context.model);
            }
            if (!context.jsonResponse) return nullptr;
            return std::make_shared<OpenAiChatJsonSink>(
                context.jsonResponse, context.model);
        }
        if (context.operation == "responses.create") {
            if (context.stream) {
                if (!context.streamWriter || !context.close) return nullptr;
                return std::make_shared<OpenAiResponsesSseSink>(
                    context.streamWriter, context.close, context.model,
                    context.nativeResponsesToolItems, context.inputTokensEstimated);
            }
            if (!context.jsonResponse) return nullptr;
            return std::make_shared<OpenAiResponsesJsonSink>(
                context.jsonResponse, context.model,
                context.inputTokensEstimated, context.nativeResponsesToolItems);
        }
        throw std::out_of_range("unknown OpenAI operation: " + context.operation);
    }
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

OpenAiProtocolModule::OpenAiProtocolModule()
    : chatAdapter_(std::make_unique<OpenAiRequestAdapter>(
          OpenAiOperation::ChatCompletions)),
      responsesAdapter_(std::make_unique<OpenAiRequestAdapter>(
          OpenAiOperation::ResponsesCreate)),
      sinkFactory_(std::make_unique<SinkFactory>()),
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

std::shared_ptr<OpenAiProtocolModule> makeOpenAiProtocolModule()
{
    return std::make_shared<OpenAiProtocolModule>();
}

}  // namespace generation::protocol::openai
