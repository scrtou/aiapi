#pragma once

#include <domain/model/ProviderCallContext.h>
#include <infrastructure/provider/retool/RetoolHttpTransport.h>
#include <infrastructure/provider/retool/RetoolProtocolHttp.h>

#include <drogon/drogon.h>
#include <json/json.h>

#include <memory>
#include <string>

namespace retool {

/** Wire adapter for Retool's agent/thread API. */
class RetoolAgentClient final
{
  public:
    explicit RetoolAgentClient(std::shared_ptr<IRetoolHttpTransport> transport);

    [[nodiscard]] protocol_http::ResponseResult fetchAgentWorkflow(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const std::string& agentId,
        const Json::Value& workspace) const;
    [[nodiscard]] protocol_http::ResponseResult saveAgentWorkflow(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const std::string& agentId,
        const Json::Value& body,
        const Json::Value& workspace) const;
    [[nodiscard]] protocol_http::ResponseResult createThread(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const std::string& agentId,
        const Json::Value& workspace) const;
    [[nodiscard]] protocol_http::ResponseResult sendTextMessage(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const std::string& agentId,
        const std::string& threadId,
        const std::string& text,
        const Json::Value& workspace) const;
    [[nodiscard]] protocol_http::ResponseResult pollRun(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const std::string& agentId,
        const std::string& runId,
        const Json::Value& workspace) const;

  private:
    std::shared_ptr<IRetoolHttpTransport> m_transport;
};

}  // namespace retool
