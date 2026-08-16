#include <infrastructure/provider/retool/RetoolWorkflowClient.h>

#include <stdexcept>
#include <infrastructure/provider/retool/RetoolProtocolHttp.h>

namespace retool {

RetoolWorkflowClient::RetoolWorkflowClient(
    std::shared_ptr<IRetoolHttpTransport> transport)
    : m_transport(std::move(transport))
{
    if (!m_transport) {
        throw std::invalid_argument("RetoolWorkflowClient requires transport");
    }
}

protocol_http::ResponseResult RetoolWorkflowClient::fetchWorkflow(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const std::string& workflowId,
    const Json::Value& workspace) const
{
    return protocol_http::sendJson(m_transport, context, baseUrl, drogon::HttpMethod::Get,
                "/api/workflow/" + workflowId, nullptr, workspace,
                "fetch workflow", 30.0);
}

protocol_http::ResponseResult RetoolWorkflowClient::saveWorkflow(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const std::string& workflowId,
    const Json::Value& body,
    const Json::Value& workspace) const
{
    return protocol_http::sendJson(m_transport, context, baseUrl, drogon::HttpMethod::Post,
                "/api/workflow/" + workflowId, &body, workspace,
                "save workflow", 60.0);
}

protocol_http::ResponseResult RetoolWorkflowClient::startRun(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const std::string& workflowId,
    const Json::Value& workspace) const
{
    Json::Value body(Json::objectValue);
    body["workflowId"] = workflowId;
    return protocol_http::sendJson(m_transport, context, baseUrl, drogon::HttpMethod::Post,
                "/api/workflow/run", &body, workspace,
                "start workflow run", 30.0);
}

protocol_http::ResponseResult RetoolWorkflowClient::pollRun(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const std::string& runId,
    const Json::Value& workspace) const
{
    return protocol_http::sendJson(m_transport, context, baseUrl, drogon::HttpMethod::Get,
                "/api/workflowRun/getBlockLevelLogs?runId=" + runId,
                nullptr, workspace, "poll workflow run", 30.0);
}

protocol_http::ResponseResult RetoolWorkflowClient::listResources(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const Json::Value& workspace) const
{
    return protocol_http::sendJson(m_transport, context, baseUrl, drogon::HttpMethod::Get,
                "/api/resources", nullptr, workspace,
                "list resources", 30.0);
}

}  // namespace retool
