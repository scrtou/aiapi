#include <application/generation/protocol/claude/ClaudeProtocolModule.h>

#include <application/generation/protocol/claude/ClaudeJsonSink.h>
#include <application/generation/protocol/claude/ClaudeRequestAdapter.h>
#include <application/generation/protocol/claude/ClaudeSseSink.h>

#include <stdexcept>
#include <utility>

namespace generation::protocol::claude {
namespace {

class SinkFactory final : public IProtocolResponseSinkFactory
{
  public:
    std::shared_ptr<IResponseSink> create(const ResponseContext& context) const override
    {
        if (context.operation != "messages.create") {
            throw std::out_of_range("unknown Claude operation: " + context.operation);
        }
        if (context.stream) {
            if (!context.streamWriter || !context.close) return nullptr;
            return std::make_shared<ClaudeSseSink>(
                context.streamWriter, context.close, context.model,
                context.inputTokensEstimated);
        }
        if (!context.jsonResponse) return nullptr;
        return std::make_shared<ClaudeJsonSink>(
            context.jsonResponse, context.model, context.inputTokensEstimated);
    }
};

class CapabilityMapper final : public ICapabilityMapper
{
  public:
    GenerationCapabilities capabilities(const std::string& operation) const override
    {
        if (operation != "messages.create") {
            throw std::out_of_range("unknown Claude operation: " + operation);
        }
        GenerationCapabilities result;
        result.text = true;
        result.images = true;
        result.streaming = true;
        result.tools = true;
        result.parallelTools = true;
        result.reasoning = true;
        result.continuity = false;
        return result;
    }
};

void requireMessagesOperation(const std::string& operation)
{
    if (operation != "messages.create") {
        throw std::out_of_range("unknown Claude operation: " + operation);
    }
}

}  // namespace

ClaudeProtocolModule::ClaudeProtocolModule()
    : requestAdapter_(std::make_unique<ClaudeRequestAdapter>()),
      responseSinkFactory_(std::make_unique<SinkFactory>()),
      capabilityMapper_(std::make_unique<CapabilityMapper>())
{
}

std::string ClaudeProtocolModule::id() const
{
    return "anthropic-messages";
}

std::string ClaudeProtocolModule::version() const
{
    return "2023-06-01";
}

std::vector<std::string> ClaudeProtocolModule::operations() const
{
    return {"messages.create"};
}

const IProtocolRequestAdapter& ClaudeProtocolModule::requestAdapter(
    const std::string& operation) const
{
    requireMessagesOperation(operation);
    return *requestAdapter_;
}

const IProtocolResponseSinkFactory& ClaudeProtocolModule::responseSinkFactory(
    const std::string& operation) const
{
    requireMessagesOperation(operation);
    return *responseSinkFactory_;
}

const ICapabilityMapper& ClaudeProtocolModule::capabilityMapper() const
{
    return *capabilityMapper_;
}

std::shared_ptr<ClaudeProtocolModule> makeClaudeProtocolModule()
{
    return std::make_shared<ClaudeProtocolModule>();
}

}  // namespace generation::protocol::claude
