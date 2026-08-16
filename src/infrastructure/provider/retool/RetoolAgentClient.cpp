#include <infrastructure/provider/retool/RetoolAgentClient.h>

#include <infrastructure/provider/retool/RetoolProtocolHttp.h>

#include <stdexcept>
#include <utility>

namespace retool {

RetoolAgentClient::RetoolAgentClient(
    std::shared_ptr<IRetoolHttpTransport> transport)
    : m_transport(std::move(transport))
{
    if (!m_transport) {
        throw std::invalid_argument("RetoolAgentClient requires transport");
    }
}

protocol_http::ResponseResult RetoolAgentClient::fetchAgentWorkflow(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const std::string& agentId,
    const Json::Value& workspace) const
{
    return protocol_http::sendJson(m_transport, context, baseUrl, drogon::HttpMethod::Get,
                "/api/workflow/" + agentId, nullptr, workspace,
                "fetch agent workflow", 30.0);
}

protocol_http::ResponseResult RetoolAgentClient::saveAgentWorkflow(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const std::string& agentId,
    const Json::Value& body,
    const Json::Value& workspace) const
{
    return protocol_http::sendJson(m_transport, context, baseUrl, drogon::HttpMethod::Post,
                "/api/workflow/" + agentId, &body, workspace,
                "save agent workflow", 60.0);
}

protocol_http::ResponseResult RetoolAgentClient::createThread(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const std::string& agentId,
    const Json::Value& workspace) const
{
    Json::Value body(Json::objectValue);
    body["name"] = "aiapi-thread";
    body["timezone"] = "UTC";
    return protocol_http::sendJson(m_transport, context, baseUrl, drogon::HttpMethod::Post,
                "/api/agents/" + agentId + "/threads",
                &body, workspace, "create agent thread", 30.0);
}

protocol_http::ResponseResult RetoolAgentClient::sendTextMessage(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const std::string& agentId,
    const std::string& threadId,
    const std::string& text,
    const Json::Value& workspace) const
{
    Json::Value body(Json::objectValue);
    body["type"] = "text";
    body["text"] = text;
    body["timezone"] = "UTC";
    return protocol_http::sendJson(m_transport, context, baseUrl, drogon::HttpMethod::Post,
                "/api/agents/" + agentId + "/threads/" + threadId + "/messages",
                &body, workspace, "send agent message", 30.0);
}

protocol_http::ResponseResult RetoolAgentClient::pollRun(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const std::string& agentId,
    const std::string& runId,
    const Json::Value& workspace) const
{
    return protocol_http::sendJson(
        m_transport, context, baseUrl, drogon::HttpMethod::Get,
        "/api/agents/" + agentId + "/logs/" + runId +
            "?startAfterUUID=00000000-0000-7000-8000-000000000000&limit=100",
        nullptr, workspace, "poll agent run", 30.0);
}

}  // namespace retool
