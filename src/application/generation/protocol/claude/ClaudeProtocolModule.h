#pragma once

#include <application/generation/protocol/common/ProtocolContracts.h>

#include <memory>

namespace generation::protocol::claude {

class ClaudeProtocolModule final : public IProtocolModule
{
  public:
    ClaudeProtocolModule();

    std::string id() const override;
    std::string version() const override;
    std::vector<std::string> operations() const override;
    const IProtocolRequestAdapter& requestAdapter(const std::string& operation) const override;
    const IProtocolResponseSinkFactory& responseSinkFactory(
        const std::string& operation) const override;
    const ICapabilityMapper& capabilityMapper() const override;

  private:
    std::unique_ptr<IProtocolRequestAdapter> requestAdapter_;
    std::unique_ptr<IProtocolResponseSinkFactory> responseSinkFactory_;
    std::unique_ptr<ICapabilityMapper> capabilityMapper_;
};

std::shared_ptr<ClaudeProtocolModule> makeClaudeProtocolModule();

}  // namespace generation::protocol::claude
