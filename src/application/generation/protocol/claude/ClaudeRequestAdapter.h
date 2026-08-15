#pragma once

#include <application/generation/protocol/common/ProtocolContracts.h>

namespace generation::protocol::claude {

class ClaudeRequestAdapter final : public IProtocolRequestAdapter
{
  public:
    AdapterResult adapt(const RawProtocolRequest& raw) const override;
};

}  // namespace generation::protocol::claude
