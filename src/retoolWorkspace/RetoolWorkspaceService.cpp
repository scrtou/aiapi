#include "RetoolWorkspaceService.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include <drogon/drogon.h>
#include <retoolWorkspace/RetoolWorkspaceManager.h>
#include <stdexcept>
#include <utils/BackgroundTaskQueue.h>

using namespace drogon;

namespace
{
std::string shellEscape(const std::string& value)
{
    std::string escaped = "'";
    for (char ch : value)
    {
        if (ch == '\'')
            escaped += "'\\''";
        else
            escaped.push_back(ch);
    }
    escaped += "'";
    return escaped;
}

RetoolWorkspaceInfo persistWorkspaceFromRoot(const Json::Value& root, std::string* errorMessage)
{
    RetoolWorkspaceInfo info = RetoolWorkspaceInfo::fromJson(root["data"]["workspace"]);
    if (info.workspaceId.empty())
    {
        info.workspaceId = !info.subdomain.empty() ? info.subdomain : info.baseUrl;
    }
    info.status = "ready";
    info.verifyStatus = "passed";
    if (!RetoolWorkspaceManager::getInstance().upsertWorkspace(info, errorMessage))
    {
        throw std::runtime_error(errorMessage ? *errorMessage : "failed to persist retool workspace");
    }
    return info;
}
}  // namespace

std::string RetoolWorkspaceService::orchestratorBaseUrl() const
{
    auto customConfig = drogon::app().getCustomConfig();
    if (customConfig.isMember("retool_workspace_service") &&
        customConfig["retool_workspace_service"].isObject())
    {
        const auto value = customConfig["retool_workspace_service"].get("orchestrator_url", "").asString();
        if (!value.empty()) return value;
    }
    const char* envValue = std::getenv("AIAPI_TOOL_ORCHESTRATOR_URL");
    if (envValue && *envValue) return std::string(envValue);
    return "http://127.0.0.1:8000";
}

std::string RetoolWorkspaceService::orchestratorApiKey() const
{
    auto customConfig = drogon::app().getCustomConfig();
    if (customConfig.isMember("retool_workspace_service") &&
        customConfig["retool_workspace_service"].isObject())
    {
        const auto value = customConfig["retool_workspace_service"].get("api_key", "").asString();
        if (!value.empty()) return value;
    }
    const char* envValue = std::getenv("AIAPI_TOOL_BEARER_KEY");
    if (envValue && *envValue) return std::string(envValue);
    return "tool_web_demo_key";
}

RetoolWorkspaceInfo RetoolWorkspaceService::provisionWorkspace(const Json::Value& requestBody, std::string* errorMessage)
{
    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "";
    const auto requestJson = Json::writeString(writerBuilder, requestBody);

    char requestTemplate[] = "/tmp/aiapi_retool_req_XXXXXX.json";
    int fd = mkstemps(requestTemplate, 5);
    if (fd < 0)
    {
        if (errorMessage) *errorMessage = "failed to create temp request file";
        throw std::runtime_error(errorMessage ? *errorMessage : "failed to create temp request file");
    }
    close(fd);
    const std::string requestPath = requestTemplate;
    {
        std::ofstream ofs(requestPath, std::ios::binary | std::ios::trunc);
        ofs << requestJson;
    }

    const auto url = orchestratorBaseUrl() + "/api/v1/workflows/retool-workspace/provision-sync";
    const auto authHeader = "Authorization: Bearer " + orchestratorApiKey();
    const std::string command =
        "curl -sS --max-time 900 -X POST " + shellEscape(url) +
        " -H " + shellEscape(authHeader) +
        " -H 'Content-Type: application/json' --data-binary @" + shellEscape(requestPath) +
        " -w '\n%{http_code}'";

    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe)
    {
        std::remove(requestPath.c_str());
        if (errorMessage) *errorMessage = "failed to start curl for aiapi_tool orchestrator";
        throw std::runtime_error(errorMessage ? *errorMessage : "failed to start curl for aiapi_tool orchestrator");
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        output += buffer;
    }
    const int exitCode = pclose(pipe);
    std::remove(requestPath.c_str());

    const auto splitPos = output.rfind('\n');
    const std::string body = splitPos == std::string::npos ? output : output.substr(0, splitPos);
    const std::string codeText = splitPos == std::string::npos ? "" : output.substr(splitPos + 1);
    int httpCode = 0;
    try
    {
        if (!codeText.empty()) httpCode = std::stoi(codeText);
    }
    catch (...)
    {
        httpCode = 0;
    }

    if (exitCode != 0 || body.empty())
    {
        const auto message = "failed to call aiapi_tool orchestrator via curl";
        if (errorMessage) *errorMessage = message;
        throw std::runtime_error(message);
    }

    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream in(body);
    if (!Json::parseFromStream(readerBuilder, in, &root, &errs))
    {
        if (errorMessage) *errorMessage = "invalid JSON returned from aiapi_tool";
        throw std::runtime_error(errorMessage ? *errorMessage : "invalid JSON returned from aiapi_tool");
    }

    if (httpCode >= 400 || !root.get("success", false).asBool())
    {
        const auto message = root.isMember("error")
                                 ? root["error"].get("message", "workspace provision failed").asString()
                                 : "workspace provision failed";
        if (errorMessage) *errorMessage = message;
        throw std::runtime_error(message);
    }

    return persistWorkspaceFromRoot(root, errorMessage);
}

void RetoolWorkspaceService::provisionWorkspaceAsync(const Json::Value& requestBody, ProvisionCallback callback)
{
    BackgroundTaskQueue::instance().enqueue(
        "retool_workspace_create",
        [requestBody, callback = std::move(callback)]() mutable {
            try
            {
                std::string error;
                auto info = RetoolWorkspaceService::getInstance().provisionWorkspace(requestBody, &error);
                callback(info, "");
            }
            catch (const std::exception& ex)
            {
                callback(std::nullopt, ex.what());
            }
        });
}
