#pragma once

#include <application/generation/protocol/common/ProtocolContracts.h>

#include <functional>
#include <memory>

namespace generation::protocol::openai {

using SinkFactoryCallback = ProtocolSinkFactoryCallback;

/** OpenAI-compatible boundary module for chat.completions and responses.create. */
class OpenAiProtocolModule final : public IProtocolModule
{
  public:
    explicit OpenAiProtocolModule(SinkFactoryCallback sinkFactory = {});

    std::string id() const override;
    std::string version() const override;
    std::vector<std::string> operations() const override;
    const IProtocolRequestAdapter& requestAdapter(const std::string& operation) const override;
    const IProtocolResponseSinkFactory& responseSinkFactory(
        const std::string& operation) const override;
    const ICapabilityMapper& capabilityMapper() const override;

  private:
    std::unique_ptr<IProtocolRequestAdapter> chatAdapter_;
    std::unique_ptr<IProtocolRequestAdapter> responsesAdapter_;
    std::unique_ptr<IProtocolResponseSinkFactory> sinkFactory_;
    std::unique_ptr<ICapabilityMapper> capabilityMapper_;
};

std::shared_ptr<OpenAiProtocolModule> makeOpenAiProtocolModule(
    SinkFactoryCallback sinkFactory = {});

}  // namespace generation::protocol::openai
