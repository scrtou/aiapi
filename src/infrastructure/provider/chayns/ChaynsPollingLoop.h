#pragma once

#include <domain/model/ProviderCallContext.h>
#include <infrastructure/provider/chayns/ChaynsClock.h>
#include <infrastructure/provider/chayns/ChaynsMessageCorrelation.h>
#include <infrastructure/provider/chayns/ChaynsProtocolClient.h>
#include <infrastructure/provider/chayns/ChaynsProviderPolicy.h>
#include <platform/result/Result.h>

#include <json/json.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace chayns {

struct PollingResult
{
    bool found = false;
    bool correlationConflict = false;
    int statusCode = 204;
    int pollCount = 0;
    std::string responseMessage;
    std::string assistantMessageId;
    std::string agentAuthorId;
    Json::Value reasoningMessages{Json::arrayValue};
};

/** Owns the cancellable Chayns message-polling protocol and correlation loop. */
class ChaynsPollingLoop final
{
  public:
    ChaynsPollingLoop(std::shared_ptr<ChaynsProtocolClient> protocolClient,
                      std::shared_ptr<IChaynsClock> clock);

    [[nodiscard]] platform::Result<PollingResult> poll(
        MessageAnchor anchor,
        const Accountinfo_st& account,
        const policy::RequestRoute& route,
        std::chrono::steady_clock::time_point requestDeadline,
        provider::ProviderCallContext& context,
        std::string_view requestId = {},
        std::string_view conversationId = {}) const;

  private:
    std::shared_ptr<ChaynsProtocolClient> m_protocolClient;
    std::shared_ptr<IChaynsClock> m_clock;
};

}  // namespace chayns
