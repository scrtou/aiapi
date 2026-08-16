#pragma once

#include <domain/model/ProviderCallContext.h>
#include <infrastructure/provider/retool/RetoolHttpTransport.h>
#include <infrastructure/provider/retool/RetoolProtocolHttp.h>

#include <drogon/drogon.h>
#include <json/json.h>

#include <memory>
#include <string>

namespace retool {

/** Wire adapter for Retool's workflow API; orchestration stays in retoolapi. */
class RetoolWorkflowClient final
{
  public:
    explicit RetoolWorkflowClient(
        std::shared_ptr<IRetoolHttpTransport> transport);

    [[nodiscard]] protocol_http::ResponseResult fetchWorkflow(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const std::string& workflowId,
        const Json::Value& workspace) const;
    [[nodiscard]] protocol_http::ResponseResult saveWorkflow(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const std::string& workflowId,
        const Json::Value& body,
        const Json::Value& workspace) const;
    [[nodiscard]] protocol_http::ResponseResult startRun(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const std::string& workflowId,
        const Json::Value& workspace) const;
    [[nodiscard]] protocol_http::ResponseResult pollRun(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const std::string& runId,
        const Json::Value& workspace) const;
    [[nodiscard]] protocol_http::ResponseResult listResources(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const Json::Value& workspace) const;

  private:
    std::shared_ptr<IRetoolHttpTransport> m_transport;
};

}  // namespace retool
