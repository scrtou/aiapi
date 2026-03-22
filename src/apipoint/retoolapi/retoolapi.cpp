#include "retoolapi.h"

#include <channelManager/channelManager.h>
#include <drogon/drogon.h>
#include <managedAccount/service/ManagedAccountService.h>
#include <retoolWorkspace/RetoolWorkspaceManager.h>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <thread>

using namespace drogon;

IMPLEMENT_RUNTIME(retoolapi, retoolapi);

namespace
{
Json::StreamWriterBuilder& compactWriter()
{
    static thread_local Json::StreamWriterBuilder writer = [] {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return builder;
    }();
    return writer;
}

std::string toCompactJson(const Json::Value& value)
{
    return Json::writeString(compactWriter(), value);
}

std::string jsonToStringOrCompactJson(const Json::Value& value, const std::string& defaultValue = "")
{
    if (value.isNull()) return defaultValue;
    if (value.isString()) return value.asString();
    return toCompactJson(value);
}

std::string trimCopy(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string envOrDefault(const char* key, const char* fallback)
{
    const char* value = std::getenv(key);
    if (value && *value) return value;
    return fallback;
}

bool isAnthropicModelName(const std::string& model)
{
    return trimCopy(model).rfind("claude-", 0) == 0;
}

size_t maxRetoolAgentBootstrapSystemPromptChars()
{
    constexpr size_t kDefaultMaxChars = 12000;
    const auto& customConfig = drogon::app().getCustomConfig();
    if (!customConfig.isObject()) return kDefaultMaxChars;

    if (customConfig.isMember("retoolapi") && customConfig["retoolapi"].isObject())
    {
        const auto& cfg = customConfig["retoolapi"];
        if (cfg.isMember("agent_bootstrap_system_prompt_max_chars"))
        {
            const auto& value = cfg["agent_bootstrap_system_prompt_max_chars"];
            if (value.isUInt64()) return static_cast<size_t>(value.asUInt64());
            if (value.isInt())
            {
                const auto v = value.asInt();
                return v <= 0 ? 0 : static_cast<size_t>(v);
            }
        }
    }

    return kDefaultMaxChars;
}

bool replaceQuotedValueAfter(std::string& input,
                             size_t startPos,
                             const std::string& fieldPrefix,
                             const std::string& replacement)
{
    const auto fieldPos = input.find(fieldPrefix, startPos);
    if (fieldPos == std::string::npos) return false;
    const auto valueStart = fieldPos + fieldPrefix.size();
    const auto valueEnd = input.find('"', valueStart);
    if (valueEnd == std::string::npos) return false;
    input.replace(valueStart, valueEnd - valueStart, replacement);
    return true;
}

class ScopedWorkspaceUsage
{
  public:
    explicit ScopedWorkspaceUsage(const std::string& workspaceId) : workspaceId_(workspaceId)
    {
        if (!workspaceId_.empty())
        {
            active_ = RetoolWorkspaceManager::getInstance().markWorkspaceUsageStarted(workspaceId_, nullptr);
        }
    }

    ~ScopedWorkspaceUsage()
    {
        if (active_ && !workspaceId_.empty())
        {
            RetoolWorkspaceManager::getInstance().markWorkspaceUsageFinished(workspaceId_, nullptr);
        }
    }

  private:
    std::string workspaceId_;
    bool active_ = false;
};
}  // namespace

retoolapi::retoolapi() = default;
retoolapi::~retoolapi() = default;

void* retoolapi::createApi()
{
    auto* api = new retoolapi();
    api->init();
    return api;
}

void retoolapi::init()
{
    modelListOpenAiFormat_ = Json::Value(Json::objectValue);
    modelListOpenAiFormat_["object"] = "list";
    modelListOpenAiFormat_["data"] = Json::Value(Json::arrayValue);

    auto appendModel = [this](const std::string& id) {
        Json::Value model(Json::objectValue);
        model["id"] = id;
        model["object"] = "model";
        modelListOpenAiFormat_["data"].append(model);
        modelInfo info;
        info.modelName = id;
        info.status = true;
        ModelInfoMap[id] = info;
    };

    appendModel("gpt-4o-mini");
    appendModel("gpt-5.4");
    appendModel("claude-opus-4-6");
    appendModel("claude-3-7-sonnet");
    appendModel("claude-sonnet-4-20250514");
    appendModel("claude-sonnet-4-6");
    appendModel("claude-sonnet-4-5-20250929");
    appendModel("claude-opus-4-5-20251101");
    appendModel("agent-gpt-5.4");
    appendModel("agent-claude-opus-4-6");
    appendModel("agent-claude-3-7-sonnet");
    appendModel("agent-claude-sonnet-4-20250514");
    appendModel("agent-claude-sonnet-4-6");
    appendModel("agent-claude-sonnet-4-5-20250929");
    appendModel("agent-claude-opus-4-5-20251101");
}

void retoolapi::checkAlivableTokens()
{
}

void retoolapi::checkModels()
{
}

Json::Value retoolapi::getModels()
{
    return modelListOpenAiFormat_;
}

std::string retoolapi::requireWorkspaceId(const session_st& session) const
{
    if (session.provider.clientInfo.isMember("workspace_id"))
    {
        return session.provider.clientInfo["workspace_id"].asString();
    }
    if (session.provider.clientInfo.isMember("workspaceId"))
    {
        return session.provider.clientInfo["workspaceId"].asString();
    }
    return "";
}

std::string retoolapi::resolveWorkspaceId(session_st& session, bool requireAgent, std::string* errorMessage) const
{
    auto explicitId = requireWorkspaceId(session);
    if (!explicitId.empty())
    {
        auto* self = const_cast<retoolapi*>(this);
        std::lock_guard<std::mutex> lock(self->threadMutex_);
        self->conversationWorkspaceMap_[session.state.conversationId] = explicitId;
        LOG_INFO << "[retoolapi] workspace selection: source=explicit"
                 << ", conversation=" << session.state.conversationId
                 << ", workspace=" << explicitId;
        return explicitId;
    }

    {
        std::lock_guard<std::mutex> lock(const_cast<retoolapi*>(this)->threadMutex_);
        auto affinityIt = conversationWorkspaceMap_.find(session.state.conversationId);
        if (affinityIt != conversationWorkspaceMap_.end() && !affinityIt->second.empty())
        {
            session.provider.clientInfo["workspace_id"] = affinityIt->second;
            LOG_INFO << "[retoolapi] workspace selection: source=conversation_affinity"
                     << ", conversation=" << session.state.conversationId
                     << ", workspace=" << affinityIt->second;
            return affinityIt->second;
        }
    }

    auto workspaces = RetoolWorkspaceManager::getInstance().listWorkspaces(errorMessage);
    std::vector<RetoolWorkspaceInfo> candidates;
    for (const auto& workspace : workspaces)
    {
        if (workspace.status == "disabled") continue;
        if (!(workspace.verifyStatus == "passed" || workspace.verifyStatus == "ready")) continue;
        if (workspace.baseUrl.empty()) continue;
        if (requireAgent)
        {
            if (workspace.agentId.empty()) continue;
        }
        else
        {
            if (workspace.workflowId.empty()) continue;
        }
        candidates.push_back(workspace);
    }

    if (candidates.empty())
    {
        if (errorMessage) *errorMessage = "no available retool workspace in pool";
        return "";
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.inUseCount != right.inUseCount) return left.inUseCount < right.inUseCount;
        const auto& leftUsed = left.lastUsedAt;
        const auto& rightUsed = right.lastUsedAt;
        if (leftUsed.empty() != rightUsed.empty()) return leftUsed.empty();
        if (leftUsed != rightUsed) return leftUsed < rightUsed;
        return left.createdAt < right.createdAt;
    });

    const auto selectedId = !candidates.front().workspaceId.empty()
        ? candidates.front().workspaceId
        : candidates.front().subdomain;
    session.provider.clientInfo["workspace_id"] = selectedId;
    {
        auto* self = const_cast<retoolapi*>(this);
        std::lock_guard<std::mutex> lock(self->threadMutex_);
        self->conversationWorkspaceMap_[session.state.conversationId] = selectedId;
    }
    LOG_INFO << "[retoolapi] workspace selection: source=pool"
             << ", conversation=" << session.state.conversationId
             << ", workspace=" << selectedId
             << ", email=" << candidates.front().email
             << ", baseUrl=" << candidates.front().baseUrl
             << ", inUseCount=" << candidates.front().inUseCount
             << ", verifyStatus=" << candidates.front().verifyStatus;
    return selectedId;
}

std::string retoolapi::buildCookieHeader(const Json::Value& workspaceJson) const
{
    std::vector<std::string> parts;
    auto append = [&parts](const std::string& name, const std::string& value) {
        if (!value.empty()) parts.push_back(name + "=" + value);
    };
    append("accessToken", workspaceJson.get("accessToken", "").asString());
    append("xsrfToken", workspaceJson.get("xsrfToken", "").asString());
    append("xsrfTokenSameSite", workspaceJson.get("xsrfToken", "").asString());
    if (workspaceJson.isMember("extraCookies") && workspaceJson["extraCookies"].isObject())
    {
        for (const auto& name : workspaceJson["extraCookies"].getMemberNames())
        {
            append(name, workspaceJson["extraCookies"][name].asString());
        }
    }
    std::ostringstream out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i) out << "; ";
        out << parts[i];
    }
    return out.str();
}

Json::Value retoolapi::parseJsonResponse(const HttpResponsePtr& resp) const
{
    if (!resp) return Json::Value(Json::nullValue);
    auto json = resp->getJsonObject();
    if (json) return *json;
    Json::Value parsed;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream in(std::string(resp->getBody()));
    if (Json::parseFromStream(reader, in, &parsed, &errs))
    {
        return parsed;
    }
    return Json::Value(Json::nullValue);
}

provider::ProviderError retoolapi::classifyHttpError(int httpStatus, const std::string& message) const
{
    if (httpStatus == 401 || httpStatus == 403)
    {
        auto err = provider::ProviderError::auth(message);
        err.httpStatusCode = httpStatus;
        return err;
    }
    if (httpStatus == 404)
    {
        provider::ProviderError err;
        err.code = provider::ProviderErrorCode::InvalidRequest;
        err.httpStatusCode = httpStatus;
        err.message = message;
        return err;
    }
    if (httpStatus == 408 || httpStatus == 504)
    {
        return provider::ProviderError::timeout(message);
    }
    if (httpStatus == 429)
    {
        return provider::ProviderError::rateLimited(message);
    }
    if (httpStatus >= 500)
    {
        provider::ProviderError err = provider::ProviderError::internal(message);
        err.httpStatusCode = httpStatus;
        return err;
    }
    provider::ProviderError err;
    err.code = provider::ProviderErrorCode::Unknown;
    err.httpStatusCode = httpStatus;
    err.message = message;
    return err;
}

HttpResponsePtr retoolapi::sendJsonRequest(
    const std::string& baseUrl,
    HttpMethod method,
    const std::string& path,
    const Json::Value* body,
    const Json::Value& workspaceJson,
    double timeoutSeconds) const
{
    auto client = HttpClient::newHttpClient(baseUrl);
    auto req = body ? HttpRequest::newHttpJsonRequest(*body) : HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(path);
    req->addHeader("accept", "application/json");
    req->addHeader("content-type", "application/json");
    req->addHeader("x-xsrf-token", workspaceJson.get("xsrfToken", "").asString());
    req->addHeader("x-retool-client-version", "3.356.0-f7a1e09 (Build 313746)");
    req->addHeader("user-agent", "Mozilla/5.0");
    req->addHeader("cookie", buildCookieHeader(workspaceJson));
    auto [result, resp] = client->sendRequest(req, timeoutSeconds);
    if (result != ReqResult::Ok || !resp)
    {
        return nullptr;
    }
    return resp;
}

std::string retoolapi::contentToText(const Json::Value& content) const
{
    if (content.isString()) return content.asString();
    if (content.isArray())
    {
        std::string out;
        for (const auto& item : content)
        {
            if (item.isObject() && item.get("type", "").asString() == "text" && item.isMember("text"))
            {
                if (!out.empty()) out += "\n";
                out += item["text"].asString();
            }
        }
        return out;
    }
    return "";
}

std::string retoolapi::buildTranscriptPrompt(const session_st& session) const
{
    std::string systemText = session.request.systemPrompt;
    std::string convoText;
    for (const auto& msg : session.provider.messageContext)
    {
        if (!msg.isObject()) continue;
        const auto role = msg.get("role", "user").asString();
        if (role == "system")
        {
            const auto text = contentToText(msg["content"]);
            if (!text.empty())
            {
                if (!systemText.empty()) systemText += "\n";
                systemText += text;
            }
            continue;
        }
        if (role == "user" || role == "assistant")
        {
            const auto text = contentToText(msg["content"]);
            if (!text.empty())
            {
                if (!convoText.empty()) convoText += "\n";
                convoText += role + ": " + text;
            }
        }
    }
    if (!session.request.message.empty())
    {
        if (!convoText.empty()) convoText += "\n";
        convoText += "user: " + session.request.message;
    }
    std::string prompt = "You are responding in a chat API. Continue the conversation naturally.";
    if (!systemText.empty()) prompt += "\n\nSystem instructions:\n" + systemText;
    if (!convoText.empty()) prompt += "\n\nConversation:\n" + convoText;
    return prompt;
}

std::string retoolapi::lastUserContent(const session_st& session) const
{
    if (!session.request.message.empty()) return session.request.message;
    const auto& ctx = session.provider.messageContext;
    if (ctx.isArray())
    {
        for (Json::ArrayIndex idx = ctx.size(); idx > 0; --idx)
        {
            const auto& item = ctx[idx - 1];
            if (item.isObject() && item.get("role", "").asString() == "user")
            {
                return contentToText(item["content"]);
            }
        }
    }
    return "";
}

std::string retoolapi::encodeJsonString(const std::string& value) const
{
    return toCompactJson(Json::Value(value)).substr(1, toCompactJson(Json::Value(value)).size() - 2);
}

Json::Value retoolapi::resolveRetoolProviderBinding(const Json::Value& workspaceJson, const std::string& model) const
{
    Json::Value binding(Json::objectValue);
    const auto lowerModel = trimCopy(model);
    const bool useAnthropic = isAnthropicModelName(lowerModel);
    if (useAnthropic)
    {
        const auto resourceName = workspaceJson.get("anthropicResourceName", "").asString();
        if (resourceName.empty())
        {
            return Json::Value(Json::nullValue);
        }
        binding["providerId"] = "retoolAIBuiltIn::anthropic";
        binding["providerName"] = "anthropic";
        binding["providerResourceName"] = resourceName;
        return binding;
    }

    const auto resourceName = workspaceJson.get("openaiResourceName", "").asString();
    if (resourceName.empty())
    {
        return Json::Value(Json::nullValue);
    }
    binding["providerId"] = "retoolAIBuiltIn::openAI";
    binding["providerName"] = "openAI";
    binding["providerResourceName"] = resourceName;
    return binding;
}

bool retoolapi::populateProviderResources(const std::string& workspaceId, Json::Value& workspaceJson) const
{
    const auto baseUrl = workspaceJson.get("baseUrl", "").asString();
    if (baseUrl.empty())
    {
        return false;
    }

    auto resourcesResp = sendJsonRequest(baseUrl, Get, "/api/resources", nullptr, workspaceJson, 30.0);
    if (!resourcesResp || resourcesResp->statusCode() >= 400)
    {
        return false;
    }

    auto resourcesJson = parseJsonResponse(resourcesResp);
    if (!resourcesJson.isMember("resources") || !resourcesJson["resources"].isArray())
    {
        return false;
    }

    for (const auto& resource : resourcesJson["resources"])
    {
        const auto type = resource.get("type", "").asString();
        if (type == "openAIProvider")
        {
            workspaceJson["openaiResourceUuid"] = resource.get("uuid", "").asString();
            workspaceJson["openaiResourceName"] = resource.get("name", "").asString();
        }
        else if (type == "anthropic")
        {
            workspaceJson["anthropicResourceUuid"] = resource.get("uuid", "").asString();
            workspaceJson["anthropicResourceName"] = resource.get("name", "").asString();
        }
    }

    RetoolWorkspaceInfo info = RetoolWorkspaceInfo::fromJson(workspaceJson);
    if (info.workspaceId.empty())
    {
        info.workspaceId = workspaceId;
    }
    if (info.baseUrl.empty())
    {
        info.baseUrl = baseUrl;
    }
    if (info.subdomain.empty())
    {
        info.subdomain = baseUrl;
    }
    RetoolWorkspaceManager::getInstance().upsertWorkspace(info, nullptr);
    return true;
}

Json::Value retoolapi::buildRetoolMeta(const std::string& workspaceId,
                                       const std::string& routeType,
                                       const std::string& resourceId,
                                       const Json::Value& binding,
                                       const std::string& model) const
{
    Json::Value meta(Json::objectValue);
    meta["workspaceId"] = workspaceId;
    meta["workspace_id"] = workspaceId;
    meta["routeType"] = routeType;
    meta["resourceId"] = resourceId;
    meta["model"] = model;
    if (binding.isObject())
    {
        meta["provider"] = binding.get("providerName", "").asString();
        meta["providerId"] = binding.get("providerId", "").asString();
        meta["resourceName"] = binding.get("providerResourceName", "").asString();
    }
    return meta;
}

bool retoolapi::replaceFirstRegex(std::string& input, const std::regex& pattern, const std::string& replacement) const
{
    std::smatch match;
    if (!std::regex_search(input, match, pattern)) return false;
    input = match.prefix().str() + replacement + match.suffix().str();
    return true;
}

Json::Value retoolapi::buildAnthropicWorkflowTemplate(const Json::Value& destinationWorkflow,
                                                      const Json::Value& workspaceJson,
                                                      const std::string& prompt,
                                                      const std::string& model) const
{
    Json::Value sourceWorkspace(Json::objectValue);
    sourceWorkspace["baseUrl"] = envOrDefault("RETOOL2_BASE_URL", "https://subscrtou1.retool.com");
    sourceWorkspace["accessToken"] = envOrDefault(
        "RETOOL2_ACCESS_TOKEN",
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ4c3JmVG9rZW4iOiI5ZjcyY2Y4NC1iZDAxLTQ2MGEtYmM3ZC04ZGZiOWI3MzNiNDEiLCJ2ZXJzaW9uIjoiMS4yIiwiaWF0IjoxNzczNjcxMTk3fQ.wshv4O2bJc9gx1LRaRPadncGBi9Un2euBcs-P5EKZgo");
    sourceWorkspace["xsrfToken"] = envOrDefault("RETOOL2_XSRF_TOKEN", "9f72cf84-bd01-460a-bc7d-8dfb9b733b41");

    const auto sourceWorkflowId = envOrDefault("RETOOL2_ANTHROPIC_WORKFLOW_ID", "268937e3-028a-4502-8c26-f8f7cb87375d");
    auto sourceResp = sendJsonRequest(
        sourceWorkspace.get("baseUrl", "").asString(),
        Get,
        "/api/workflow/" + sourceWorkflowId,
        nullptr,
        sourceWorkspace,
        30.0);
    if (!sourceResp || sourceResp->statusCode() != k200OK)
    {
        return Json::Value(Json::nullValue);
    }

    auto sourceJson = parseJsonResponse(sourceResp);
    if (!sourceJson.isMember("workflow") || !sourceJson["workflow"].isObject())
    {
        return Json::Value(Json::nullValue);
    }

    Json::Value cloned = sourceJson["workflow"];
    static const std::vector<std::string> copyKeys = {
        "id", "saveId", "apiKey", "folderId", "createdAt", "updatedAt", "createdBy", "accessLevel", "releaseId"};
    for (const auto& key : copyKeys)
    {
        if (destinationWorkflow.isMember(key))
        {
            cloned[key] = destinationWorkflow[key];
        }
    }
    if (destinationWorkflow.isMember("id")) cloned["id"] = destinationWorkflow["id"];
    if (destinationWorkflow.isMember("organizationId")) cloned["organizationId"] = destinationWorkflow["organizationId"];
    if (destinationWorkflow.isMember("name")) cloned["name"] = destinationWorkflow["name"];
    if (destinationWorkflow.isMember("description")) cloned["description"] = destinationWorkflow["description"];

    auto serialized = toCompactJson(cloned);
    const auto sourceOrgId = sourceJson["workflow"].isMember("organizationId")
        ? trimCopy(toCompactJson(sourceJson["workflow"]["organizationId"]))
        : std::string();
    const auto destinationOrgId = destinationWorkflow.isMember("organizationId")
        ? trimCopy(toCompactJson(destinationWorkflow["organizationId"]))
        : std::string();
    if (!sourceOrgId.empty() && !destinationOrgId.empty())
    {
        auto pos = serialized.find(sourceOrgId);
        while (pos != std::string::npos)
        {
            serialized.replace(pos, sourceOrgId.size(), destinationOrgId);
            pos = serialized.find(sourceOrgId, pos + destinationOrgId.size());
        }
    }

    const auto destinationAnthropicResource = workspaceJson.get("anthropicResourceName", "").asString();
    for (const auto& sourceResource : {
             envOrDefault("RETOOL2_ANTHROPIC_RESOURCE", "16aef8a4-100d-4668-869f-f726d13947d5"),
             envOrDefault("RETOOL2_OPENAI_RESOURCE", "5872d1e8-a6e6-4e39-af14-b94b3c584e2e")})
    {
        auto pos = serialized.find(sourceResource);
        while (!destinationAnthropicResource.empty() && pos != std::string::npos)
        {
            serialized.replace(pos, sourceResource.size(), destinationAnthropicResource);
            pos = serialized.find(sourceResource, pos + destinationAnthropicResource.size());
        }
    }

    Json::Value parsed;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream in(serialized);
    if (!Json::parseFromStream(reader, in, &parsed, &errs))
    {
        return Json::Value(Json::nullValue);
    }

    return patchWorkflowTemplate(parsed, workspaceJson, prompt, model);
}

Json::Value retoolapi::patchWorkflowTemplate(const Json::Value& workflow, const Json::Value& workspaceJson, const std::string& prompt, const std::string& model) const
{
    Json::Value patched = workflow;
    auto data = patched.get("templateData", "").asString();
    replaceFirstRegex(data, std::regex("\"instruction\",\".*?\""), "\"instruction\",\"" + encodeJsonString(prompt) + "\"");
    replaceFirstRegex(data, std::regex("\"model\",\".*?\""), "\"model\",\"" + encodeJsonString(model) + "\"");
    auto binding = resolveRetoolProviderBinding(workspaceJson, model);
    if (binding.isObject())
    {
        const auto providerId = binding.get("providerId", "").asString();
        const auto providerName = binding.get("providerName", "").asString();
        const auto providerResourceName = binding.get("providerResourceName", "").asString();
        replaceFirstRegex(data, std::regex("\"providerId\",\".*?\""), "\"providerId\",\"" + encodeJsonString(providerId) + "\"");
        replaceFirstRegex(data, std::regex("\"providerName\",\".*?\""), "\"providerName\",\"" + encodeJsonString(providerName) + "\"");
        if (!providerResourceName.empty())
        {
            if (!replaceFirstRegex(data, std::regex("\"providerResourceName\",\".*?\""), "\"providerResourceName\",\"" + encodeJsonString(providerResourceName) + "\""))
            {
                replaceFirstRegex(
                    data,
                    std::regex("\"providerId\",\"" + encodeJsonString(providerId) + "\""),
                    "\"providerId\",\"" + encodeJsonString(providerId) + "\",\"providerResourceName\",\"" + encodeJsonString(providerResourceName) + "\"");
            }
        }

        const auto desiredSubtype = isAnthropicModelName(model) ? "AnthropicQuery" : "OpenAIProviderQuery";
        auto subtypePos = data.find("\"OpenAIProviderQuery\"");
        if (subtypePos == std::string::npos)
        {
            subtypePos = data.find("\"AnthropicQuery\"");
        }
        if (subtypePos != std::string::npos)
        {
            const auto subtypeStart = subtypePos + 1;
            const auto subtypeEnd = data.find('"', subtypeStart);
            if (subtypeEnd != std::string::npos)
            {
                data.replace(subtypeStart, subtypeEnd - subtypeStart, desiredSubtype);
            }
            replaceQuotedValueAfter(
                data,
                subtypePos,
                "\"^1;\",\"",
                encodeJsonString(providerResourceName));
        }
    }
    patched["templateData"] = data;
    return patched;
}

Json::Value retoolapi::patchAgentTemplate(const Json::Value& workflow, const Json::Value& workspaceJson, const std::string& model) const
{
    Json::Value patched = workflow;
    auto data = patched.get("templateData", "").asString();
    const auto binding = resolveRetoolProviderBinding(workspaceJson, model);
    const auto providerId = binding.get("providerId", "").asString();
    const auto providerName = binding.get("providerName", "").asString();
    const auto providerResourceName = binding.get("providerResourceName", "").asString();
    replaceFirstRegex(data, std::regex("\"providerId\",\".*?\""), "\"providerId\",\"" + encodeJsonString(providerId) + "\"");
    replaceFirstRegex(data, std::regex("\"providerName\",\".*?\""), "\"providerName\",\"" + encodeJsonString(providerName) + "\"");
    if (!providerResourceName.empty())
    {
        if (!replaceFirstRegex(data, std::regex("\"providerResourceName\",\".*?\""), "\"providerResourceName\",\"" + encodeJsonString(providerResourceName) + "\""))
        {
            replaceFirstRegex(
                data,
                std::regex("\"providerId\",\"" + encodeJsonString(providerId) + "\""),
                "\"providerId\",\"" + encodeJsonString(providerId) + "\",\"providerResourceName\",\"" + encodeJsonString(providerResourceName) + "\"");
        }
    }
    replaceFirstRegex(data, std::regex("\"model\",\".*?\""), "\"model\",\"" + encodeJsonString(model) + "\"");
    patched["templateData"] = data;
    return patched;
}

provider::ProviderResult retoolapi::requestWorkflow(session_st& session)
{
    std::string resolveError;
    const auto workspaceId = resolveWorkspaceId(session, false, &resolveError);
    if (workspaceId.empty())
    {
        return provider::ProviderResult::fail(provider::ProviderError::auth(resolveError.empty() ? "workspaceId is required for retoolapi" : resolveError));
    }
    ScopedWorkspaceUsage usageGuard(workspaceId);
    std::string error;
    auto ctx = ManagedAccountService::getInstance().buildExecutionContext(
        ManagedAccountKind::RetoolWorkspace, workspaceId, &error);
    if (!ctx)
    {
        return provider::ProviderResult::fail(provider::ProviderError::auth(error.empty() ? "retool workspace not found" : error));
    }
    Json::Value workspace = ctx->data;
    const std::string baseUrl = workspace.get("baseUrl", "").asString();
    const std::string workflowId = workspace.get("workflowId", "").asString();
    LOG_INFO << "[retoolapi] resolved workspace context: conversation=" << session.state.conversationId
             << ", workspace=" << workspaceId
             << ", email=" << workspace.get("email", "").asString()
             << ", baseUrl=" << baseUrl
             << ", route=workflow";
    if (baseUrl.empty() || workflowId.empty())
    {
        return provider::ProviderResult::fail(provider::ProviderError::internal("retool workspace is missing workflow configuration"));
    }

    auto workflowResp = sendJsonRequest(baseUrl, Get, "/api/workflow/" + workflowId, nullptr, workspace);
    if (!workflowResp)
    {
        return provider::ProviderResult::fail(provider::ProviderError::network("failed to fetch retool workflow"));
    }
    auto workflowJson = parseJsonResponse(workflowResp);
    if (workflowResp->statusCode() != k200OK || !workflowJson.isMember("workflow"))
    {
        return provider::ProviderResult::fail(classifyHttpError(static_cast<int>(workflowResp->statusCode()), std::string(workflowResp->getBody())));
    }

    const std::string requestedModel = session.request.model.empty() ? "gpt-4o-mini" : session.request.model;
    auto binding = resolveRetoolProviderBinding(workspace, requestedModel);
    if (!binding.isObject())
    {
        populateProviderResources(workspaceId, workspace);
        binding = resolveRetoolProviderBinding(workspace, requestedModel);
    }
    if (!binding.isObject())
    {
        return provider::ProviderResult::fail(provider::ProviderError::internal("matching retool provider resource not found for requested model"));
    }

    const auto prompt = buildTranscriptPrompt(session);
    Json::Value patched;
    if (isAnthropicModelName(requestedModel))
    {
        patched = buildAnthropicWorkflowTemplate(workflowJson["workflow"], workspace, prompt, requestedModel);
    }
    if (!patched.isObject())
    {
        patched = patchWorkflowTemplate(
            workflowJson["workflow"],
            workspace,
            prompt,
            requestedModel);
    }
    auto saveResp = sendJsonRequest(baseUrl, Post, "/api/workflow/" + workflowId, &patched, workspace, 60.0);
    if (!saveResp || saveResp->statusCode() >= 400)
    {
        return provider::ProviderResult::fail(
            classifyHttpError(saveResp ? static_cast<int>(saveResp->statusCode()) : 503,
                              saveResp ? std::string(saveResp->getBody()) : std::string("failed to save retool workflow")));
    }

    Json::Value runBody(Json::objectValue);
    runBody["workflowId"] = workflowId;
    auto runResp = sendJsonRequest(baseUrl, Post, "/api/workflow/run", &runBody, workspace);
    if (!runResp)
    {
        return provider::ProviderResult::fail(provider::ProviderError::network("failed to start retool workflow run"));
    }
    auto runJson = parseJsonResponse(runResp);
    if (runResp->statusCode() >= 400 || !runJson.isMember("id"))
    {
        return provider::ProviderResult::fail(classifyHttpError(static_cast<int>(runResp->statusCode()), std::string(runResp->getBody())));
    }
    const std::string runId = runJson["id"].asString();
    for (int i = 0; i < 120; ++i)
    {
        auto pollResp = sendJsonRequest(baseUrl, Get, "/api/workflowRun/getBlockLevelLogs?runId=" + runId, nullptr, workspace);
        if (!pollResp)
        {
            return provider::ProviderResult::fail(provider::ProviderError::network("failed to poll retool workflow run"));
        }
        auto pollJson = parseJsonResponse(pollResp);
        const auto code1 = pollJson["blockLevelLogs"]["code1"];
        const auto status = code1.get("status", "").asString();
        if (status == "SUCCESS")
        {
            std::string content = jsonToStringOrCompactJson(code1["output"]["data"], "");
            auto result = provider::ProviderResult::success(trimCopy(content));
            result.meta = buildRetoolMeta(workspaceId, "workflow", workflowId, binding, requestedModel);
            return result;
        }
        if (status == "FAILED")
        {
            return provider::ProviderResult::fail(provider::ProviderError::internal(
                jsonToStringOrCompactJson(code1["output"]["error"], "workflow failed")));
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return provider::ProviderResult::fail(provider::ProviderError::timeout("retool workflow run timed out"));
}

provider::ProviderResult retoolapi::requestAgent(session_st& session)
{
    std::string resolveError;
    const auto workspaceId = resolveWorkspaceId(session, true, &resolveError);
    if (workspaceId.empty())
    {
        return provider::ProviderResult::fail(provider::ProviderError::auth(resolveError.empty() ? "workspaceId is required for retoolapi" : resolveError));
    }
    ScopedWorkspaceUsage usageGuard(workspaceId);
    std::string error;
    auto ctx = ManagedAccountService::getInstance().buildExecutionContext(
        ManagedAccountKind::RetoolWorkspace, workspaceId, &error);
    if (!ctx)
    {
        return provider::ProviderResult::fail(provider::ProviderError::auth(error.empty() ? "retool workspace not found" : error));
    }
    Json::Value workspace = ctx->data;
    const std::string baseUrl = workspace.get("baseUrl", "").asString();
    const std::string agentId = workspace.get("agentId", "").asString();
    LOG_INFO << "[retoolapi] resolved workspace context: conversation=" << session.state.conversationId
             << ", workspace=" << workspaceId
             << ", email=" << workspace.get("email", "").asString()
             << ", baseUrl=" << baseUrl
             << ", route=agent";
    if (baseUrl.empty() || agentId.empty())
    {
        return provider::ProviderResult::fail(provider::ProviderError::internal("retool workspace is missing agent configuration"));
    }

    auto workflowResp = sendJsonRequest(baseUrl, Get, "/api/workflow/" + agentId, nullptr, workspace);
    if (!workflowResp)
    {
        return provider::ProviderResult::fail(provider::ProviderError::network("failed to fetch retool agent workflow"));
    }
    auto workflowJson = parseJsonResponse(workflowResp);
    if (workflowResp->statusCode() != k200OK || !workflowJson.isMember("workflow"))
    {
        return provider::ProviderResult::fail(classifyHttpError(static_cast<int>(workflowResp->statusCode()), std::string(workflowResp->getBody())));
    }

    std::string requestedModel = session.request.model;
    if (requestedModel.rfind("agent-", 0) == 0)
    {
        requestedModel = requestedModel.substr(6);
    }
    if (requestedModel.empty())
    {
        requestedModel = "gpt-5.4";
    }
    auto binding = resolveRetoolProviderBinding(workspace, requestedModel);
    if (!binding.isObject())
    {
        populateProviderResources(workspaceId, workspace);
        binding = resolveRetoolProviderBinding(workspace, requestedModel);
    }
    if (!binding.isObject())
    {
        return provider::ProviderResult::fail(provider::ProviderError::internal("matching retool provider resource not found for requested model"));
    }
    auto patched = patchAgentTemplate(workflowJson["workflow"], workspace, requestedModel);
    auto saveResp = sendJsonRequest(baseUrl, Post, "/api/workflow/" + agentId, &patched, workspace, 60.0);
    if (!saveResp || saveResp->statusCode() >= 400)
    {
        return provider::ProviderResult::fail(
            classifyHttpError(saveResp ? static_cast<int>(saveResp->statusCode()) : 503,
                              saveResp ? std::string(saveResp->getBody()) : std::string("failed to save retool agent workflow")));
    }

    auto createThread = [&](bool persistMapping) -> std::optional<std::string> {
        Json::Value threadBody(Json::objectValue);
        threadBody["name"] = "aiapi-thread";
        threadBody["timezone"] = "UTC";
        auto threadResp = sendJsonRequest(baseUrl, Post, "/api/agents/" + agentId + "/threads", &threadBody, workspace);
        if (!threadResp)
        {
            LOG_ERROR << "[retoolapi] createThread failed: no response, workspace=" << workspaceId
                      << ", conversation=" << session.state.conversationId;
            return std::nullopt;
        }
        auto threadJson = parseJsonResponse(threadResp);
        if (threadResp->statusCode() >= 400 || !threadJson.isMember("id"))
        {
            LOG_ERROR << "[retoolapi] createThread failed: status=" << static_cast<int>(threadResp->statusCode())
                      << ", body=" << threadResp->getBody()
                      << ", workspace=" << workspaceId
                      << ", conversation=" << session.state.conversationId;
            return std::nullopt;
        }
        const auto newThreadId = threadJson["id"].asString();
        LOG_INFO << "[retoolapi] createThread success: workspace=" << workspaceId
                 << ", conversation=" << session.state.conversationId
                 << ", threadId=" << newThreadId
                 << ", persistMapping=" << (persistMapping ? 1 : 0);
        if (persistMapping)
        {
            std::lock_guard<std::mutex> lock(threadMutex_);
            agentThreadMap_[session.state.conversationId] = newThreadId;
        }
        return newThreadId;
    };

    auto sendThreadTextMessage = [&](const std::string& targetThreadId,
                                     const std::string& text) -> HttpResponsePtr {
        Json::Value messageBody(Json::objectValue);
        messageBody["type"] = "text";
        messageBody["text"] = text;
        messageBody["timezone"] = "UTC";
        return sendJsonRequest(
            baseUrl,
            Post,
            "/api/agents/" + agentId + "/threads/" + targetThreadId + "/messages",
            &messageBody,
            workspace);
    };

    auto waitForAgentRun = [&](const std::string& runId, std::string* errorMessage) -> bool {
        for (int i = 0; i < 180; ++i)
        {
            auto pollResp = sendJsonRequest(
                baseUrl,
                Get,
                "/api/agents/" + agentId +
                    "/logs/" + runId + "?startAfterUUID=00000000-0000-7000-8000-000000000000&limit=100",
                nullptr,
                workspace);
            if (!pollResp)
            {
                if (errorMessage) *errorMessage = "failed to poll retool agent logs during thread replay";
                return false;
            }
            auto pollJson = parseJsonResponse(pollResp);
            const auto status = pollJson.get("status", "").asString();
            if (status == "COMPLETED")
            {
                return true;
            }
            if (status == "FAILED")
            {
                std::string message = "agent replay failed";
                const auto trace = pollJson["trace"];
                if (trace.isArray() && !trace.empty())
                {
                    const auto last = trace[static_cast<int>(trace.size()) - 1];
                    message = last["data"].get("error", message).asString();
                }
                if (errorMessage) *errorMessage = message;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (errorMessage) *errorMessage = "retool agent replay timed out";
        return false;
    };

    auto replayHistoryToThread = [&](const std::string& targetThreadId, std::string* errorMessage) -> bool {
        int replayedSystem = 0;
        int replayedUser = 0;
        int replayedAssistant = 0;
        const auto bootstrapSystem = trimCopy(session.request.systemPrompt);
        const auto bootstrapSystemMaxChars = maxRetoolAgentBootstrapSystemPromptChars();
        if (!bootstrapSystem.empty() &&
            (bootstrapSystemMaxChars == 0 || bootstrapSystem.size() <= bootstrapSystemMaxChars))
        {
            LOG_INFO << "[retoolapi] replay bootstrap system prompt: workspace=" << workspaceId
                     << ", conversation=" << session.state.conversationId
                     << ", threadId=" << targetThreadId
                     << ", chars=" << bootstrapSystem.size();
            auto bootstrapResp = sendThreadTextMessage(
                targetThreadId,
                "Session bootstrap. Treat the following as durable system instructions for this conversation.\n\n" + bootstrapSystem);
            if (!bootstrapResp)
            {
                if (errorMessage) *errorMessage = "failed to bootstrap system prompt into recreated retool thread";
                return false;
            }
            auto bootstrapJson = parseJsonResponse(bootstrapResp);
            if (bootstrapResp->statusCode() >= 400)
            {
                if (errorMessage) *errorMessage = std::string(bootstrapResp->getBody());
                return false;
            }
            const std::string bootstrapRunId =
                bootstrapJson.get("agentRunId", "").asString().empty()
                    ? bootstrapJson["content"].get("runId", "").asString()
                    : bootstrapJson.get("agentRunId", "").asString();
            if (!bootstrapRunId.empty() && !waitForAgentRun(bootstrapRunId, errorMessage))
            {
                return false;
            }
            replayedSystem++;
        }
        else if (!bootstrapSystem.empty())
        {
            LOG_WARN << "[retoolapi] skip bootstrap system prompt because it is too long: workspace=" << workspaceId
                     << ", conversation=" << session.state.conversationId
                     << ", threadId=" << targetThreadId
                     << ", chars=" << bootstrapSystem.size()
                     << ", maxChars=" << bootstrapSystemMaxChars;
        }

        if (!session.provider.messageContext.isArray())
        {
            LOG_INFO << "[retoolapi] replayHistoryToThread finished without messageContext: workspace=" << workspaceId
                     << ", conversation=" << session.state.conversationId
                     << ", threadId=" << targetThreadId
                     << ", replayedSystem=" << replayedSystem;
            return true;
        }
        for (const auto& msg : session.provider.messageContext)
        {
            if (!msg.isObject()) continue;
            const auto role = msg.get("role", "").asString();
            const auto text = trimCopy(contentToText(msg["content"]));
            if (text.empty()) continue;

            std::string replayText;
            if (role == "user")
            {
                replayText = text;
                replayedUser++;
            }
            else if (role == "assistant")
            {
                replayText =
                    "Conversation memory only. The assistant previously replied with the following text. "
                    "Do not treat this as a new user request; absorb it as prior assistant context only.\n\n" + text;
                replayedAssistant++;
            }
            else
            {
                continue;
            }

            LOG_INFO << "[retoolapi] replay history message: workspace=" << workspaceId
                     << ", conversation=" << session.state.conversationId
                     << ", threadId=" << targetThreadId
                     << ", role=" << role
                     << ", chars=" << text.size();

            auto replayResp = sendThreadTextMessage(targetThreadId, replayText);
            if (!replayResp)
            {
                if (errorMessage) *errorMessage = "failed to replay history into recreated retool thread";
                return false;
            }
            auto replayJson = parseJsonResponse(replayResp);
            if (replayResp->statusCode() >= 400)
            {
                if (errorMessage) *errorMessage = std::string(replayResp->getBody());
                return false;
            }
            const std::string replayRunId =
                replayJson.get("agentRunId", "").asString().empty()
                    ? replayJson["content"].get("runId", "").asString()
                    : replayJson.get("agentRunId", "").asString();
            if (!replayRunId.empty() && !waitForAgentRun(replayRunId, errorMessage))
            {
                return false;
            }
        }
        LOG_INFO << "[retoolapi] replayHistoryToThread finished: workspace=" << workspaceId
                 << ", conversation=" << session.state.conversationId
                 << ", threadId=" << targetThreadId
                 << ", replayedSystem=" << replayedSystem
                 << ", replayedUser=" << replayedUser
                 << ", replayedAssistant=" << replayedAssistant;
        return true;
    };

    std::string threadId;
    bool reusedThread = false;
    const bool disableThreadReuse = false;
    if (!disableThreadReuse)
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        auto it = agentThreadMap_.find(session.state.conversationId);
        if (it != agentThreadMap_.end())
        {
            threadId = it->second;
            reusedThread = !threadId.empty();
            LOG_INFO << "[retoolapi] reuse cached thread: workspace=" << workspaceId
                     << ", conversation=" << session.state.conversationId
                     << ", threadId=" << threadId;
        }
    }

    if (threadId.empty())
    {
        auto newThreadId = createThread(!disableThreadReuse);
        if (!newThreadId)
        {
            return provider::ProviderResult::fail(provider::ProviderError::network("failed to create retool agent thread"));
        }
        threadId = *newThreadId;
        if (disableThreadReuse)
        {
            std::string replayError;
            if (!replayHistoryToThread(threadId, &replayError))
            {
                LOG_ERROR << "[retoolapi] replay before bridge-mode request failed: workspace=" << workspaceId
                          << ", conversation=" << session.state.conversationId
                          << ", threadId=" << threadId
                          << ", error=" << replayError;
                return provider::ProviderResult::fail(provider::ProviderError::internal(
                    replayError.empty() ? "failed to replay history into fresh retool bridge thread" : replayError));
            }
        }
    }

    const auto currentUserText = lastUserContent(session);
    auto messageResp = sendThreadTextMessage(threadId, currentUserText);
    if (!messageResp)
    {
        return provider::ProviderResult::fail(provider::ProviderError::network("failed to send retool agent message"));
    }
    if (reusedThread && messageResp->statusCode() == k404NotFound)
    {
        const auto body = std::string(messageResp->getBody());
        if (body.find("Thread not found") != std::string::npos)
        {
            LOG_WARN << "[retoolapi] cached thread missing upstream, recreating: workspace=" << workspaceId
                     << ", conversation=" << session.state.conversationId
                     << ", oldThreadId=" << threadId;
            if (!disableThreadReuse)
            {
                std::lock_guard<std::mutex> lock(threadMutex_);
                agentThreadMap_.erase(session.state.conversationId);
            }
            auto replacementThreadId = createThread(!disableThreadReuse);
            if (!replacementThreadId)
            {
                return provider::ProviderResult::fail(provider::ProviderError::network("failed to recreate missing retool agent thread"));
            }
            threadId = *replacementThreadId;
            LOG_INFO << "[retoolapi] recreated missing thread: workspace=" << workspaceId
                     << ", conversation=" << session.state.conversationId
                     << ", newThreadId=" << threadId;
            std::string replayError;
            if (!replayHistoryToThread(threadId, &replayError))
            {
                LOG_ERROR << "[retoolapi] replay after thread recreation failed: workspace=" << workspaceId
                          << ", conversation=" << session.state.conversationId
                          << ", threadId=" << threadId
                          << ", error=" << replayError;
                return provider::ProviderResult::fail(provider::ProviderError::internal(
                    replayError.empty() ? "failed to replay history into recreated retool thread" : replayError));
            }
            messageResp = sendThreadTextMessage(threadId, currentUserText);
            if (!messageResp)
            {
                return provider::ProviderResult::fail(provider::ProviderError::network("failed to resend retool agent message after recreating thread"));
            }
            LOG_INFO << "[retoolapi] resent current message after thread recreation: workspace=" << workspaceId
                     << ", conversation=" << session.state.conversationId
                     << ", threadId=" << threadId
                     << ", chars=" << currentUserText.size();
        }
    }
    auto messageJson = parseJsonResponse(messageResp);
    if (messageResp->statusCode() >= 400)
    {
        return provider::ProviderResult::fail(classifyHttpError(static_cast<int>(messageResp->statusCode()), std::string(messageResp->getBody())));
    }
    const std::string runId =
        messageJson.get("agentRunId", "").asString().empty()
            ? messageJson["content"].get("runId", "").asString()
            : messageJson.get("agentRunId", "").asString();
    if (runId.empty())
    {
        return provider::ProviderResult::fail(provider::ProviderError::internal("missing retool agent run id"));
    }

    for (int i = 0; i < 180; ++i)
    {
        auto pollResp = sendJsonRequest(
            baseUrl,
            Get,
            "/api/agents/" + agentId +
                "/logs/" + runId + "?startAfterUUID=00000000-0000-7000-8000-000000000000&limit=100",
            nullptr,
            workspace);
        if (!pollResp)
        {
            return provider::ProviderResult::fail(provider::ProviderError::network("failed to poll retool agent logs"));
        }
        auto pollJson = parseJsonResponse(pollResp);
        const auto status = pollJson.get("status", "").asString();
        if (status == "COMPLETED")
        {
            const auto trace = pollJson["trace"];
            if (trace.isArray() && !trace.empty())
            {
                const auto last = trace[static_cast<int>(trace.size()) - 1];
                const auto content = last["data"]["data"].get("content", "").asString();
                auto result = provider::ProviderResult::success(trimCopy(content));
                result.meta = buildRetoolMeta(workspaceId, "agent", agentId, binding, requestedModel);
                return result;
            }
            auto result = provider::ProviderResult::success("");
            result.meta = buildRetoolMeta(workspaceId, "agent", agentId, binding, requestedModel);
            return result;
        }
        if (status == "FAILED")
        {
            const auto trace = pollJson["trace"];
            std::string message = "agent failed";
            if (trace.isArray() && !trace.empty())
            {
                const auto last = trace[static_cast<int>(trace.size()) - 1];
                message = last["data"].get("error", message).asString();
            }
            return provider::ProviderResult::fail(provider::ProviderError::internal(message));
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return provider::ProviderResult::fail(provider::ProviderError::timeout("retool agent run timed out"));
}

provider::ProviderResult retoolapi::generate(session_st& session)
{
    for (const auto& channel : ChannelManager::getInstance().getChannelList())
    {
        if (channel.channelName == "retoolapi" && !channel.channelStatus)
        {
            return provider::ProviderResult::fail(
                provider::ProviderError::auth("retoolapi channel is disabled"));
        }
    }

    const std::string requestedModel = session.request.model;
    if (requestedModel.rfind("agent-", 0) == 0)
    {
        return requestAgent(session);
    }
    return requestWorkflow(session);
}

void retoolapi::afterResponseProcess(session_st&)
{
}

void retoolapi::eraseChatinfoMap(std::string conversationId)
{
    std::lock_guard<std::mutex> lock(threadMutex_);
    agentThreadMap_.erase(conversationId);
    conversationWorkspaceMap_.erase(conversationId);
}

void retoolapi::transferThreadContext(const std::string& oldId, const std::string& newId)
{
    if (oldId.empty() || newId.empty() || oldId == newId) return;
    std::lock_guard<std::mutex> lock(threadMutex_);
    auto it = agentThreadMap_.find(oldId);
    if (it != agentThreadMap_.end())
    {
        agentThreadMap_[newId] = it->second;
        agentThreadMap_.erase(it);
    }
    auto workspaceIt = conversationWorkspaceMap_.find(oldId);
    if (workspaceIt != conversationWorkspaceMap_.end())
    {
        conversationWorkspaceMap_[newId] = workspaceIt->second;
        conversationWorkspaceMap_.erase(workspaceIt);
    }
}
