#include <apipoint/retoolapi/retoolapi.h>
#include <retoolWorkspace/RetoolWorkspaceJsonCodec.h>

#include <drogon/drogon.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>

using namespace drogon;


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
    ScopedWorkspaceUsage(workspace::IRetoolWorkspaceUseCase* workspaces,
                         const std::string& workspaceId)
        : workspaces_(workspaces), workspaceId_(workspaceId)
    {
        if (workspaces_ && !workspaceId_.empty())
        {
            active_ = workspaces_->markUsageStarted(workspaceId_, nullptr);
        }
    }

    ~ScopedWorkspaceUsage()
    {
        if (active_ && workspaces_ && !workspaceId_.empty())
        {
            workspaces_->markUsageFinished(workspaceId_, nullptr);
        }
    }

  private:
    workspace::IRetoolWorkspaceUseCase* workspaces_;
    std::string workspaceId_;
    bool active_ = false;
};

std::optional<platform::Error> interruptionError(
    const provider::ProviderCallContext& context)
{
    if (context.isCancelled()) {
        return platform::Error::cancelled("Retool provider request cancelled");
    }
    if (context.deadlineExceeded()) {
        return platform::Error::timeout("Retool provider request deadline exceeded");
    }
    return std::nullopt;
}

size_t configuredHistoryLimit(const char* key, size_t fallback)
{
    constexpr size_t kMaximumConfiguredBytes = 8 * 1024 * 1024;
    const auto& customConfig = drogon::app().getCustomConfig();
    if (!customConfig.isObject() ||
        !customConfig.isMember("history_replay") ||
        !customConfig["history_replay"].isObject()) {
        return fallback;
    }

    const auto& value = customConfig["history_replay"][key];
    if (value.isUInt64()) {
        return static_cast<size_t>(std::min<Json::UInt64>(
            value.asUInt64(), static_cast<Json::UInt64>(kMaximumConfiguredBytes)));
    }
    if (value.isInt()) {
        const int configured = value.asInt();
        if (configured >= 0) {
            return static_cast<size_t>(std::min<int>(
                configured, static_cast<int>(kMaximumConfiguredBytes)));
        }
    }
    return fallback;
}
}  // namespace

retoolapi::retoolapi(std::shared_ptr<retool::IRetoolHttpTransport> transport,
                     std::shared_ptr<retool::IRetoolClock> clock,
                     IManagedAccountContextResolver& accounts,
                     workspace::IRetoolWorkspaceUseCase& workspaces,
                     IChannelCatalog& channels,
                     provider::ProviderBase::FailureObserver failureObserver)
    : provider::ProviderBase(std::move(failureObserver)),
      transport_(std::move(transport)), clock_(std::move(clock))
{
    if (!transport_)
    {
        throw std::invalid_argument("retoolapi requires a non-null HTTP transport");
    }
    if (!clock_)
    {
        throw std::invalid_argument("retoolapi requires a non-null clock");
    }
    accounts_ = &accounts;
    workspaces_ = &workspaces;
    channels_ = &channels;
}

retoolapi::~retoolapi() = default;

platform::Result<void> retoolapi::initialize()
{
    modelCatalog_.models.clear();
    auto appendModel = [this](const std::string& id) {
        ProviderModel model;
        model.id = id;
        modelCatalog_.models.push_back(std::move(model));
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
    return platform::Result<void>::success();
}

provider::ProviderCapabilities retoolapi::capabilities() const noexcept
{
    return provider::ProviderCapabilities{/*nativeToolCalls=*/false,
                                          /*upstreamHistory=*/true,
                                          /*supportsImages=*/false};
}

ProviderModelCatalog retoolapi::getModels()
{
    return modelCatalog_;
}

std::string retoolapi::requireWorkspaceId(
    const provider::ProviderRequest& request) const
{
    for (const char* key : {"workspace_id", "workspaceId"}) {
        const auto it = request.routingHints.find(key);
        if (it != request.routingHints.end() && !it->second.empty()) {
            return it->second;
        }
    }
    return "";
}

std::string retoolapi::resolveWorkspaceId(const provider::ProviderRequest& request,
                                          bool requireAgent,
                                          std::string* errorMessage)
{
    if (!workspaces_)
    {
        if (errorMessage) *errorMessage = "retool workspace service unavailable";
        return "";
    }
    auto explicitId = requireWorkspaceId(request);
    if (!explicitId.empty())
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        conversationWorkspaceMap_[request.conversationId] = explicitId;
        LOG_INFO << "[retoolapi] workspace selection: source=explicit"
                 << ", conversation=" << request.conversationId
                 << ", workspace=" << explicitId;
        return explicitId;
    }

    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        auto affinityIt = conversationWorkspaceMap_.find(request.conversationId);
        if (affinityIt != conversationWorkspaceMap_.end() && !affinityIt->second.empty())
        {
            LOG_INFO << "[retoolapi] workspace selection: source=conversation_affinity"
                     << ", conversation=" << request.conversationId
                     << ", workspace=" << affinityIt->second;
            return affinityIt->second;
        }
    }

    auto workspaces = workspaces_->list(errorMessage);
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
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        conversationWorkspaceMap_[request.conversationId] = selectedId;
    }
    LOG_INFO << "[retoolapi] workspace selection: source=pool"
             << ", conversation=" << request.conversationId
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

platform::Error retoolapi::classifyHttpError(
    int httpStatus, const std::string& message) const
{
    platform::ErrorCode code = platform::ErrorCode::ProviderError;
    if (httpStatus == 401) {
        code = platform::ErrorCode::Unauthorized;
    } else if (httpStatus == 403) {
        code = platform::ErrorCode::Forbidden;
    } else if (httpStatus == 400 || httpStatus == 404 || httpStatus == 422) {
        code = platform::ErrorCode::BadRequest;
    } else if (httpStatus == 408 || httpStatus == 504) {
        code = platform::ErrorCode::Timeout;
    } else if (httpStatus == 429) {
        code = platform::ErrorCode::RateLimited;
    }
    return platform::Error(code, message, {}, {}, httpStatus);
}

retool::HttpResult retoolapi::sendWithinContext(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const HttpRequestPtr& request,
    double maximumTimeoutSeconds) const
{
    const auto remaining = context.remaining();
    const double remainingSeconds = static_cast<double>(remaining.count()) / 1000.0;
    const double timeoutSeconds = std::min(maximumTimeoutSeconds, remainingSeconds);
    if (timeoutSeconds <= 0.0 || context.isCancelled()) {
        return {ReqResult::BadResponse, nullptr};
    }
    return transport_->send(baseUrl, request, timeoutSeconds);
}

HttpResponsePtr retoolapi::sendJsonRequest(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    HttpMethod method,
    const std::string& path,
    const Json::Value* body,
    const Json::Value& workspaceJson,
    double timeoutSeconds) const
{
    auto req = body ? HttpRequest::newHttpJsonRequest(*body) : HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(path);
    req->addHeader("accept", "application/json");
    req->addHeader("content-type", "application/json");
    req->addHeader("x-xsrf-token", workspaceJson.get("xsrfToken", "").asString());
    req->addHeader("x-retool-client-version", "3.356.0-f7a1e09 (Build 313746)");
    req->addHeader("user-agent", "Mozilla/5.0");
    req->addHeader("cookie", buildCookieHeader(workspaceJson));
    auto [result, resp] = sendWithinContext(context, baseUrl, req, timeoutSeconds);
    if (interruptionError(context)) {
        return nullptr;
    }
    if (result != ReqResult::Ok || !resp)
    {
        return nullptr;
    }
    return resp;
}

bool retoolapi::sleepWithinContext(
    const provider::ProviderCallContext& context,
    std::chrono::milliseconds duration) const
{
    if (interruptionError(context)) return false;
    const auto remaining = context.remaining();
    const auto sleepFor = std::min(duration, remaining);
    if (sleepFor <= std::chrono::milliseconds::zero()) return false;
    clock_->sleepFor(sleepFor);
    return !interruptionError(context).has_value();
}

std::string retoolapi::buildTranscriptPrompt(
    const provider::ProviderRequest& request) const
{
    std::string systemText = request.systemPrompt;
    std::string convoText;
    for (const auto& message : request.messages)
    {
        if (message.role == provider::ProviderMessageRole::System)
        {
            if (!message.text.empty())
            {
                if (!systemText.empty()) systemText += "\n";
                systemText += message.text;
            }
            continue;
        }
        if (message.role == provider::ProviderMessageRole::User ||
            message.role == provider::ProviderMessageRole::Assistant)
        {
            if (!message.text.empty())
            {
                if (!convoText.empty()) convoText += "\n";
                convoText += message.role == provider::ProviderMessageRole::User
                    ? "user: " + message.text
                    : "assistant: " + message.text;
            }
        }
    }
    if (!request.input.empty())
    {
        if (!convoText.empty()) convoText += "\n";
        convoText += "user: " + request.input;
    }
    std::string prompt = "You are responding in a chat API. Continue the conversation naturally.";
    if (!systemText.empty()) prompt += "\n\nSystem instructions:\n" + systemText;
    if (!convoText.empty()) prompt += "\n\nConversation:\n" + convoText;
    return prompt;
}

std::string retoolapi::lastUserContent(
    const provider::ProviderRequest& request) const
{
    if (!request.input.empty()) return request.input;
    for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
        if (it->role == provider::ProviderMessageRole::User && !it->text.empty()) {
            return it->text;
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

bool retoolapi::populateProviderResources(
    const provider::ProviderCallContext& context,
    const std::string& workspaceId,
    Json::Value& workspaceJson) const
{
    const auto baseUrl = workspaceJson.get("baseUrl", "").asString();
    if (baseUrl.empty())
    {
        return false;
    }

    auto resourcesResp = sendJsonRequest(
        context, baseUrl, Get, "/api/resources", nullptr, workspaceJson, 30.0);
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

    RetoolWorkspaceInfo info = retoolworkspacecodec::fromJson(workspaceJson);
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
    if (!workspaces_ || !workspaces_->upsert(info, nullptr))
    {
        return false;
    }
    return true;
}

provider::ProviderMetadata retoolapi::buildRetoolMeta(const std::string& workspaceId,
                                                      const std::string& routeType,
                                                      const std::string& resourceId,
                                                      const Json::Value& binding,
                                                      const std::string& model) const
{
    provider::ProviderMetadata meta;
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

Json::Value retoolapi::buildAnthropicWorkflowTemplate(
    const provider::ProviderCallContext& context,
    const Json::Value& destinationWorkflow,
    const Json::Value& workspaceJson,
    const std::string& prompt,
    const std::string& model) const
{
    const auto sourceBaseUrl = envOrDefault("RETOOL2_BASE_URL", "");
    const auto sourceAccessToken = envOrDefault("RETOOL2_ACCESS_TOKEN", "");
    const auto sourceXsrfToken = envOrDefault("RETOOL2_XSRF_TOKEN", "");
    const auto sourceWorkflowId = envOrDefault("RETOOL2_ANTHROPIC_WORKFLOW_ID", "");
    // Cloning is optional.  Never carry a live workspace URL or credential as
    // a source-code fallback; when the explicit source is not configured the
    // caller safely patches the destination workflow instead.
    if (sourceBaseUrl.empty() || sourceAccessToken.empty() ||
        sourceXsrfToken.empty() || sourceWorkflowId.empty())
    {
        return Json::Value(Json::nullValue);
    }

    Json::Value sourceWorkspace(Json::objectValue);
    sourceWorkspace["baseUrl"] = sourceBaseUrl;
    sourceWorkspace["accessToken"] = sourceAccessToken;
    sourceWorkspace["xsrfToken"] = sourceXsrfToken;

    auto sourceResp = sendJsonRequest(
        context,
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
             envOrDefault("RETOOL2_ANTHROPIC_RESOURCE", ""),
             envOrDefault("RETOOL2_OPENAI_RESOURCE", "")})
    {
        if (sourceResource.empty()) continue;
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

platform::Result<provider::ProviderResponse> retoolapi::requestWorkflow(
    const provider::ProviderRequest& request,
    provider::ProviderCallContext& context)
{
    using Result = platform::Result<provider::ProviderResponse>;
    if (const auto interrupted = interruptionError(context)) {
        return Result::failure(*interrupted);
    }

    std::string resolveError;
    const auto workspaceId = resolveWorkspaceId(request, false, &resolveError);
    if (workspaceId.empty()) {
        return Result::failure(platform::Error::unauthorized(
            resolveError.empty() ? "workspaceId is required for retoolapi" : resolveError));
    }
    ScopedWorkspaceUsage usageGuard(workspaces_, workspaceId);

    std::string error;
    if (!accounts_) {
        return Result::failure(platform::Error::internal(
            "managed account service unavailable"));
    }
    auto accountContext = accounts_->buildExecutionContext(
        ManagedAccountKind::RetoolWorkspace, workspaceId, &error);
    if (!accountContext) {
        return Result::failure(platform::Error::unauthorized(
            error.empty() ? "retool workspace not found" : error));
    }

    Json::Value workspace = accountContext->data;
    const std::string baseUrl = workspace.get("baseUrl", "").asString();
    const std::string workflowId = workspace.get("workflowId", "").asString();
    LOG_INFO << "[retoolapi] resolved workspace context: conversation=" << request.conversationId
             << ", workspace=" << workspaceId
             << ", email=" << workspace.get("email", "").asString()
             << ", baseUrl=" << baseUrl
             << ", route=workflow";
    if (baseUrl.empty() || workflowId.empty()) {
        return Result::failure(platform::Error::internal(
            "retool workspace is missing workflow configuration"));
    }

    auto workflowResp = sendJsonRequest(
        context, baseUrl, Get, "/api/workflow/" + workflowId, nullptr, workspace);
    if (const auto interrupted = interruptionError(context)) {
        return Result::failure(*interrupted);
    }
    if (!workflowResp) {
        return Result::failure(platform::Error::providerError(
            "failed to fetch retool workflow"));
    }
    auto workflowJson = parseJsonResponse(workflowResp);
    if (workflowResp->statusCode() != k200OK || !workflowJson.isMember("workflow")) {
        if (workflowResp->statusCode() == k200OK) {
            return Result::failure(platform::Error::providerError(
                "Retool workflow response is invalid", {},
                static_cast<int>(workflowResp->statusCode())));
        }
        return Result::failure(classifyHttpError(
            static_cast<int>(workflowResp->statusCode()),
            std::string(workflowResp->getBody())));
    }

    const std::string requestedModel = request.model.empty() ? "gpt-4o-mini" : request.model;
    auto binding = resolveRetoolProviderBinding(workspace, requestedModel);
    if (!binding.isObject()) {
        populateProviderResources(context, workspaceId, workspace);
        if (const auto interrupted = interruptionError(context)) {
            return Result::failure(*interrupted);
        }
        binding = resolveRetoolProviderBinding(workspace, requestedModel);
    }
    if (!binding.isObject()) {
        return Result::failure(platform::Error::providerError(
            "matching retool provider resource not found for requested model"));
    }

    const auto prompt = buildTranscriptPrompt(request);
    Json::Value patched;
    if (isAnthropicModelName(requestedModel)) {
        patched = buildAnthropicWorkflowTemplate(
            context, workflowJson["workflow"], workspace, prompt, requestedModel);
        if (const auto interrupted = interruptionError(context)) {
            return Result::failure(*interrupted);
        }
    }
    if (!patched.isObject()) {
        patched = patchWorkflowTemplate(
            workflowJson["workflow"], workspace, prompt, requestedModel);
    }

    auto saveResp = sendJsonRequest(
        context, baseUrl, Post, "/api/workflow/" + workflowId, &patched, workspace, 60.0);
    if (const auto interrupted = interruptionError(context)) {
        return Result::failure(*interrupted);
    }
    if (!saveResp || saveResp->statusCode() >= 400) {
        return Result::failure(saveResp
            ? classifyHttpError(static_cast<int>(saveResp->statusCode()),
                                std::string(saveResp->getBody()))
            : platform::Error::providerError("failed to save retool workflow"));
    }

    Json::Value runBody(Json::objectValue);
    runBody["workflowId"] = workflowId;
    auto runResp = sendJsonRequest(
        context, baseUrl, Post, "/api/workflow/run", &runBody, workspace);
    if (const auto interrupted = interruptionError(context)) {
        return Result::failure(*interrupted);
    }
    if (!runResp) {
        return Result::failure(platform::Error::providerError(
            "failed to start retool workflow run"));
    }
    auto runJson = parseJsonResponse(runResp);
    if (runResp->statusCode() >= 400 || !runJson.isMember("id")) {
        if (runResp->statusCode() < 400) {
            return Result::failure(platform::Error::providerError(
                "Retool workflow run response is invalid", {},
                static_cast<int>(runResp->statusCode())));
        }
        return Result::failure(classifyHttpError(
            static_cast<int>(runResp->statusCode()), std::string(runResp->getBody())));
    }

    const std::string runId = runJson["id"].asString();
    for (int i = 0; i < 120; ++i) {
        if (const auto interrupted = interruptionError(context)) {
            return Result::failure(*interrupted);
        }
        auto pollResp = sendJsonRequest(
            context, baseUrl, Get,
            "/api/workflowRun/getBlockLevelLogs?runId=" + runId,
            nullptr, workspace);
        if (const auto interrupted = interruptionError(context)) {
            return Result::failure(*interrupted);
        }
        if (!pollResp) {
            return Result::failure(platform::Error::providerError(
                "failed to poll retool workflow run"));
        }
        if (pollResp->statusCode() >= 400) {
            return Result::failure(classifyHttpError(
                static_cast<int>(pollResp->statusCode()), std::string(pollResp->getBody())));
        }
        auto pollJson = parseJsonResponse(pollResp);
        const auto code1 = pollJson["blockLevelLogs"]["code1"];
        const auto status = code1.get("status", "").asString();
        if (status == "SUCCESS") {
            provider::ProviderResponse response;
            response.text = trimCopy(jsonToStringOrCompactJson(code1["output"]["data"], ""));
            response.meta = buildRetoolMeta(
                workspaceId, "workflow", workflowId, binding, requestedModel);
            return Result::success(std::move(response));
        }
        if (status == "FAILED") {
            return Result::failure(platform::Error::providerError(
                jsonToStringOrCompactJson(code1["output"]["error"], "workflow failed")));
        }
        if (!sleepWithinContext(context, std::chrono::seconds(1))) {
            if (const auto interrupted = interruptionError(context)) {
                return Result::failure(*interrupted);
            }
            return Result::failure(platform::Error::timeout(
                "retool workflow run timed out"));
        }
    }
    return Result::failure(platform::Error::timeout("retool workflow run timed out"));
}

platform::Result<provider::ProviderResponse> retoolapi::requestAgent(
    const provider::ProviderRequest& request,
    provider::ProviderCallContext& context)
{
    using Result = platform::Result<provider::ProviderResponse>;
    if (const auto interrupted = interruptionError(context)) {
        return Result::failure(*interrupted);
    }

    std::string resolveError;
    const auto workspaceId = resolveWorkspaceId(request, true, &resolveError);
    if (workspaceId.empty()) {
        return Result::failure(platform::Error::unauthorized(
            resolveError.empty() ? "workspaceId is required for retoolapi" : resolveError));
    }
    ScopedWorkspaceUsage usageGuard(workspaces_, workspaceId);

    std::string error;
    if (!accounts_) {
        return Result::failure(platform::Error::internal(
            "managed account service unavailable"));
    }
    auto accountContext = accounts_->buildExecutionContext(
        ManagedAccountKind::RetoolWorkspace, workspaceId, &error);
    if (!accountContext) {
        return Result::failure(platform::Error::unauthorized(
            error.empty() ? "retool workspace not found" : error));
    }

    Json::Value workspace = accountContext->data;
    const std::string baseUrl = workspace.get("baseUrl", "").asString();
    const std::string agentId = workspace.get("agentId", "").asString();
    LOG_INFO << "[retoolapi] resolved workspace context: conversation=" << request.conversationId
             << ", workspace=" << workspaceId
             << ", email=" << workspace.get("email", "").asString()
             << ", baseUrl=" << baseUrl
             << ", route=agent";
    if (baseUrl.empty() || agentId.empty()) {
        return Result::failure(platform::Error::internal(
            "retool workspace is missing agent configuration"));
    }

    auto workflowResp = sendJsonRequest(
        context, baseUrl, Get, "/api/workflow/" + agentId, nullptr, workspace);
    if (const auto interrupted = interruptionError(context)) {
        return Result::failure(*interrupted);
    }
    if (!workflowResp) {
        return Result::failure(platform::Error::providerError(
            "failed to fetch retool agent workflow"));
    }
    auto workflowJson = parseJsonResponse(workflowResp);
    if (workflowResp->statusCode() != k200OK || !workflowJson.isMember("workflow")) {
        if (workflowResp->statusCode() == k200OK) {
            return Result::failure(platform::Error::providerError(
                "Retool agent workflow response is invalid", {},
                static_cast<int>(workflowResp->statusCode())));
        }
        return Result::failure(classifyHttpError(
            static_cast<int>(workflowResp->statusCode()),
            std::string(workflowResp->getBody())));
    }

    std::string requestedModel = request.model;
    if (requestedModel.rfind("agent-", 0) == 0) {
        requestedModel = requestedModel.substr(6);
    }
    if (requestedModel.empty()) {
        requestedModel = "gpt-5.4";
    }
    auto binding = resolveRetoolProviderBinding(workspace, requestedModel);
    if (!binding.isObject()) {
        populateProviderResources(context, workspaceId, workspace);
        if (const auto interrupted = interruptionError(context)) {
            return Result::failure(*interrupted);
        }
        binding = resolveRetoolProviderBinding(workspace, requestedModel);
    }
    if (!binding.isObject()) {
        return Result::failure(platform::Error::providerError(
            "matching retool provider resource not found for requested model"));
    }
    auto patched = patchAgentTemplate(workflowJson["workflow"], workspace, requestedModel);
    auto saveResp = sendJsonRequest(
        context, baseUrl, Post, "/api/workflow/" + agentId, &patched, workspace, 60.0);
    if (const auto interrupted = interruptionError(context)) {
        return Result::failure(*interrupted);
    }
    if (!saveResp || saveResp->statusCode() >= 400) {
        return Result::failure(saveResp
            ? classifyHttpError(static_cast<int>(saveResp->statusCode()),
                                std::string(saveResp->getBody()))
            : platform::Error::providerError("failed to save retool agent workflow"));
    }

    auto createThread = [&]() -> platform::Result<std::string> {
        if (const auto interrupted = interruptionError(context)) {
            return platform::Result<std::string>::failure(*interrupted);
        }
        Json::Value threadBody(Json::objectValue);
        threadBody["name"] = "aiapi-thread";
        threadBody["timezone"] = "UTC";
        auto threadResp = sendJsonRequest(
            context, baseUrl, Post, "/api/agents/" + agentId + "/threads",
            &threadBody, workspace);
        if (const auto interrupted = interruptionError(context)) {
            return platform::Result<std::string>::failure(*interrupted);
        }
        if (!threadResp) {
            return platform::Result<std::string>::failure(platform::Error::providerError(
                "failed to create retool agent thread"));
        }
        auto threadJson = parseJsonResponse(threadResp);
        if (threadResp->statusCode() >= 400 || !threadJson.isMember("id")) {
            if (threadResp->statusCode() < 400) {
                return platform::Result<std::string>::failure(platform::Error::providerError(
                    "Retool agent thread response is invalid", {},
                    static_cast<int>(threadResp->statusCode())));
            }
            return platform::Result<std::string>::failure(classifyHttpError(
                static_cast<int>(threadResp->statusCode()), std::string(threadResp->getBody())));
        }
        const auto newThreadId = threadJson["id"].asString();
        if (newThreadId.empty()) {
            return platform::Result<std::string>::failure(platform::Error::providerError(
                "Retool agent thread response has an empty id"));
        }
        LOG_INFO << "[retoolapi] createThread success: workspace=" << workspaceId
                 << ", conversation=" << request.conversationId
                 << ", threadId=" << newThreadId;
        {
            std::lock_guard<std::mutex> lock(threadMutex_);
            agentThreadMap_[request.conversationId] = newThreadId;
        }
        return platform::Result<std::string>::success(newThreadId);
    };

    auto sendThreadTextMessage = [&](const std::string& targetThreadId,
                                     const std::string& text)
        -> platform::Result<HttpResponsePtr> {
        if (const auto interrupted = interruptionError(context)) {
            return platform::Result<HttpResponsePtr>::failure(*interrupted);
        }
        Json::Value messageBody(Json::objectValue);
        messageBody["type"] = "text";
        messageBody["text"] = text;
        messageBody["timezone"] = "UTC";
        auto response = sendJsonRequest(
            context, baseUrl,
            Post, "/api/agents/" + agentId + "/threads/" + targetThreadId + "/messages",
            &messageBody, workspace);
        if (const auto interrupted = interruptionError(context)) {
            return platform::Result<HttpResponsePtr>::failure(*interrupted);
        }
        if (!response) {
            return platform::Result<HttpResponsePtr>::failure(platform::Error::providerError(
                "failed to send retool agent message"));
        }
        return platform::Result<HttpResponsePtr>::success(std::move(response));
    };

    auto waitForAgentRun = [&](const std::string& runId) -> platform::Result<void> {
        for (int i = 0; i < 180; ++i) {
            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<void>::failure(*interrupted);
            }
            auto pollResp = sendJsonRequest(
                context, baseUrl, Get,
                "/api/agents/" + agentId +
                    "/logs/" + runId +
                    "?startAfterUUID=00000000-0000-7000-8000-000000000000&limit=100",
                nullptr, workspace);
            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<void>::failure(*interrupted);
            }
            if (!pollResp) {
                return platform::Result<void>::failure(platform::Error::providerError(
                    "failed to poll retool agent logs during thread replay"));
            }
            if (pollResp->statusCode() >= 400) {
                return platform::Result<void>::failure(classifyHttpError(
                    static_cast<int>(pollResp->statusCode()), std::string(pollResp->getBody())));
            }
            auto pollJson = parseJsonResponse(pollResp);
            const auto status = pollJson.get("status", "").asString();
            if (status == "COMPLETED") {
                return platform::Result<void>::success();
            }
            if (status == "FAILED") {
                std::string message = "agent replay failed";
                const auto trace = pollJson["trace"];
                if (trace.isArray() && !trace.empty()) {
                    const auto last = trace[static_cast<int>(trace.size()) - 1];
                    message = last["data"].get("error", message).asString();
                }
                return platform::Result<void>::failure(platform::Error::providerError(message));
            }
            if (!sleepWithinContext(context, std::chrono::seconds(1))) {
                if (const auto interrupted = interruptionError(context)) {
                    return platform::Result<void>::failure(*interrupted);
                }
                return platform::Result<void>::failure(platform::Error::timeout(
                    "retool agent replay timed out"));
            }
        }
        return platform::Result<void>::failure(platform::Error::timeout(
            "retool agent replay timed out"));
    };

    auto replayHistoryToThread = [&](const std::string& targetThreadId)
        -> platform::Result<void> {
        int replayedSystem = 0;
        int replayedUser = 0;
        int replayedAssistant = 0;
        const auto bootstrapSystem = trimCopy(request.systemPrompt);
        const auto bootstrapSystemMaxChars = maxRetoolAgentBootstrapSystemPromptChars();
        if (!bootstrapSystem.empty() &&
            (bootstrapSystemMaxChars == 0 || bootstrapSystem.size() <= bootstrapSystemMaxChars)) {
            LOG_INFO << "[retoolapi] replay bootstrap system prompt: workspace=" << workspaceId
                     << ", conversation=" << request.conversationId
                     << ", threadId=" << targetThreadId
                     << ", chars=" << bootstrapSystem.size();
            auto bootstrap = sendThreadTextMessage(
                targetThreadId,
                "Session bootstrap. Treat the following as durable system instructions for this conversation.\n\n" +
                    bootstrapSystem);
            if (!bootstrap) return platform::Result<void>::failure(bootstrap.error());
            const auto& bootstrapResp = bootstrap.value();
            auto bootstrapJson = parseJsonResponse(bootstrapResp);
            if (bootstrapResp->statusCode() >= 400) {
                return platform::Result<void>::failure(classifyHttpError(
                    static_cast<int>(bootstrapResp->statusCode()),
                    std::string(bootstrapResp->getBody())));
            }
            const std::string bootstrapRunId =
                bootstrapJson.get("agentRunId", "").asString().empty()
                    ? bootstrapJson["content"].get("runId", "").asString()
                    : bootstrapJson.get("agentRunId", "").asString();
            if (!bootstrapRunId.empty()) {
                const auto waited = waitForAgentRun(bootstrapRunId);
                if (!waited) return waited;
            }
            ++replayedSystem;
        } else if (!bootstrapSystem.empty()) {
            LOG_WARN << "[retoolapi] skip bootstrap system prompt because it is too long: workspace="
                     << workspaceId << ", conversation=" << request.conversationId
                     << ", threadId=" << targetThreadId
                     << ", chars=" << bootstrapSystem.size()
                     << ", maxChars=" << bootstrapSystemMaxChars;
        }

        constexpr size_t kDefaultReplayRequestBytes = 256 * 1024;
        constexpr size_t kDefaultReplayMessageBytes = 128 * 1024;
        const size_t replayRequestBudget = configuredHistoryLimit(
            "max_request_bytes", kDefaultReplayRequestBytes);
        const size_t replayMessageBudget = configuredHistoryLimit(
            "max_message_bytes", kDefaultReplayMessageBytes);
        const std::string currentHistoryMessage = request.rawInput.empty()
            ? request.input
            : request.rawInput;
        // Preserve the established history_replay contract: zero request budget
        // disables replay entirely (it does not mean "unlimited").
        if (replayRequestBudget == 0) {
            LOG_INFO << "[retoolapi] recreated thread history disabled by request budget: workspace="
                     << workspaceId << ", conversation=" << request.conversationId
                     << ", threadId=" << targetThreadId;
            return platform::Result<void>::success();
        }

        std::vector<const provider::ProviderMessage*> selected;
        size_t selectedBytes = 0;
        bool skippedDuplicateCurrent = false;
        for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
            if (it->role != provider::ProviderMessageRole::User &&
                it->role != provider::ProviderMessageRole::Assistant) {
                continue;
            }
            const auto text = trimCopy(it->text);
            if (text.empty()) continue;
            if (!skippedDuplicateCurrent &&
                it->role == provider::ProviderMessageRole::User &&
                text == currentHistoryMessage) {
                skippedDuplicateCurrent = true;
                continue;
            }
            std::string replayText = it->role == provider::ProviderMessageRole::User
                ? text
                : "Conversation memory only. The assistant previously replied with the following text. "
                  "Do not treat this as a new user request; absorb it as prior assistant context only.\n\n" + text;
            if ((replayMessageBudget > 0 && replayText.size() > replayMessageBudget) ||
                (replayRequestBudget > 0 && replayText.size() > replayRequestBudget - selectedBytes)) {
                continue;
            }
            selected.push_back(&*it);
            selectedBytes += replayText.size();
        }
        std::reverse(selected.begin(), selected.end());
        LOG_INFO << "[retoolapi] recreated thread history budget: workspace=" << workspaceId
                 << ", conversation=" << request.conversationId
                 << ", threadId=" << targetThreadId
                 << ", selected=" << selected.size()
                 << ", selectedBytes=" << selectedBytes
                 << ", skippedDuplicateCurrent=" << skippedDuplicateCurrent;

        for (const auto* message : selected) {
            const auto role = message->role;
            const auto text = trimCopy(message->text);
            std::string replayText = role == provider::ProviderMessageRole::User
                ? text
                : "Conversation memory only. The assistant previously replied with the following text. "
                  "Do not treat this as a new user request; absorb it as prior assistant context only.\n\n" + text;
            auto replay = sendThreadTextMessage(targetThreadId, replayText);
            if (!replay) return platform::Result<void>::failure(replay.error());
            const auto& replayResp = replay.value();
            if (static_cast<int>(replayResp->statusCode()) == 413) {
                LOG_WARN << "[retoolapi] skip replay history message after upstream HTTP 413: workspace="
                         << workspaceId << ", conversation=" << request.conversationId
                         << ", threadId=" << targetThreadId
                         << ", bytes=" << replayText.size();
                continue;
            }
            if (replayResp->statusCode() >= 400) {
                return platform::Result<void>::failure(classifyHttpError(
                    static_cast<int>(replayResp->statusCode()),
                    std::string(replayResp->getBody())));
            }
            const auto replayJson = parseJsonResponse(replayResp);
            const std::string replayRunId =
                replayJson.get("agentRunId", "").asString().empty()
                    ? replayJson["content"].get("runId", "").asString()
                    : replayJson.get("agentRunId", "").asString();
            if (!replayRunId.empty()) {
                const auto waited = waitForAgentRun(replayRunId);
                if (!waited) return waited;
            }
            if (role == provider::ProviderMessageRole::User) ++replayedUser;
            if (role == provider::ProviderMessageRole::Assistant) ++replayedAssistant;
        }
        LOG_INFO << "[retoolapi] replayHistoryToThread finished: workspace=" << workspaceId
                 << ", conversation=" << request.conversationId
                 << ", threadId=" << targetThreadId
                 << ", replayedSystem=" << replayedSystem
                 << ", replayedUser=" << replayedUser
                 << ", replayedAssistant=" << replayedAssistant;
        return platform::Result<void>::success();
    };

    std::string threadId;
    bool reusedThread = false;
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        const auto it = agentThreadMap_.find(request.conversationId);
        if (it != agentThreadMap_.end()) {
            threadId = it->second;
            reusedThread = !threadId.empty();
            LOG_INFO << "[retoolapi] reuse cached thread: workspace=" << workspaceId
                     << ", conversation=" << request.conversationId
                     << ", threadId=" << threadId;
        }
    }
    if (threadId.empty()) {
        const auto created = createThread();
        if (!created) return Result::failure(created.error());
        threadId = created.value();
    }

    const auto currentUserText = lastUserContent(request);
    auto message = sendThreadTextMessage(threadId, currentUserText);
    if (!message) return Result::failure(message.error());
    HttpResponsePtr messageResp = message.value();
    if (reusedThread && messageResp->statusCode() == k404NotFound &&
        std::string(messageResp->getBody()).find("Thread not found") != std::string::npos) {
        LOG_WARN << "[retoolapi] cached thread missing upstream, recreating: workspace=" << workspaceId
                 << ", conversation=" << request.conversationId
                 << ", oldThreadId=" << threadId;
        {
            std::lock_guard<std::mutex> lock(threadMutex_);
            agentThreadMap_.erase(request.conversationId);
        }
        const auto replacement = createThread();
        if (!replacement) return Result::failure(replacement.error());
        threadId = replacement.value();
        const auto replay = replayHistoryToThread(threadId);
        if (!replay) return Result::failure(replay.error());
        message = sendThreadTextMessage(threadId, currentUserText);
        if (!message) return Result::failure(message.error());
        messageResp = message.value();
        LOG_INFO << "[retoolapi] resent current message after thread recreation: workspace="
                 << workspaceId << ", conversation=" << request.conversationId
                 << ", threadId=" << threadId
                 << ", chars=" << currentUserText.size();
    }

    if (messageResp->statusCode() >= 400) {
        return Result::failure(classifyHttpError(
            static_cast<int>(messageResp->statusCode()), std::string(messageResp->getBody())));
    }
    const auto messageJson = parseJsonResponse(messageResp);
    const std::string runId = messageJson.get("agentRunId", "").asString().empty()
        ? messageJson["content"].get("runId", "").asString()
        : messageJson.get("agentRunId", "").asString();
    if (runId.empty()) {
        return Result::failure(platform::Error::providerError(
            "missing retool agent run id"));
    }

    for (int i = 0; i < 180; ++i) {
        if (const auto interrupted = interruptionError(context)) {
            return Result::failure(*interrupted);
        }
        auto pollResp = sendJsonRequest(
            context, baseUrl, Get,
            "/api/agents/" + agentId +
                "/logs/" + runId +
                "?startAfterUUID=00000000-0000-7000-8000-000000000000&limit=100",
            nullptr, workspace);
        if (const auto interrupted = interruptionError(context)) {
            return Result::failure(*interrupted);
        }
        if (!pollResp) {
            return Result::failure(platform::Error::providerError(
                "failed to poll retool agent logs"));
        }
        if (pollResp->statusCode() >= 400) {
            return Result::failure(classifyHttpError(
                static_cast<int>(pollResp->statusCode()), std::string(pollResp->getBody())));
        }
        const auto pollJson = parseJsonResponse(pollResp);
        const auto status = pollJson.get("status", "").asString();
        if (status == "COMPLETED") {
            provider::ProviderResponse response;
            const auto trace = pollJson["trace"];
            if (trace.isArray() && !trace.empty()) {
                const auto last = trace[static_cast<int>(trace.size()) - 1];
                response.text = trimCopy(last["data"]["data"].get("content", "").asString());
            }
            response.meta = buildRetoolMeta(
                workspaceId, "agent", agentId, binding, requestedModel);
            return Result::success(std::move(response));
        }
        if (status == "FAILED") {
            const auto trace = pollJson["trace"];
            std::string messageText = "agent failed";
            if (trace.isArray() && !trace.empty()) {
                const auto last = trace[static_cast<int>(trace.size()) - 1];
                messageText = last["data"].get("error", messageText).asString();
            }
            return Result::failure(platform::Error::providerError(messageText));
        }
        if (!sleepWithinContext(context, std::chrono::seconds(1))) {
            if (const auto interrupted = interruptionError(context)) {
                return Result::failure(*interrupted);
            }
            return Result::failure(platform::Error::timeout(
                "retool agent run timed out"));
        }
    }
    return Result::failure(platform::Error::timeout("retool agent run timed out"));
}

platform::Result<provider::ProviderResponse> retoolapi::doGenerate(
    const provider::ProviderRequest& request,
    provider::ProviderCallContext& context)
{
    using Result = platform::Result<provider::ProviderResponse>;
    if (const auto interrupted = interruptionError(context)) {
        return Result::failure(*interrupted);
    }
    if (modelCatalog_.models.empty()) {
        return Result::failure(platform::Error::internal(
            "retoolapi provider has not been initialized"));
    }
    if (!channels_) {
        return Result::failure(platform::Error::internal("channel catalog unavailable"));
    }
    for (const auto& channel : channels_->listChannels()) {
        if (channel.channelName == "retoolapi" && !channel.channelStatus) {
            return Result::failure(platform::Error::unauthorized(
                "retoolapi channel is disabled"));
        }
    }

    if (request.model.rfind("agent-", 0) == 0) {
        return requestAgent(request, context);
    }
    return requestWorkflow(request, context);
}

platform::Result<void> retoolapi::eraseThreadContext(
    const std::string& conversationId)
{
    std::lock_guard<std::mutex> lock(threadMutex_);
    agentThreadMap_.erase(conversationId);
    conversationWorkspaceMap_.erase(conversationId);
    return platform::Result<void>::success();
}

platform::Result<void> retoolapi::transferThreadContext(
    const std::string& oldId, const std::string& newId)
{
    if (oldId.empty() || newId.empty() || oldId == newId) {
        return platform::Result<void>::success();
    }
    std::lock_guard<std::mutex> lock(threadMutex_);
    const auto it = agentThreadMap_.find(oldId);
    if (it != agentThreadMap_.end()) {
        agentThreadMap_[newId] = it->second;
        agentThreadMap_.erase(it);
    }
    const auto workspaceIt = conversationWorkspaceMap_.find(oldId);
    if (workspaceIt != conversationWorkspaceMap_.end()) {
        conversationWorkspaceMap_[newId] = workspaceIt->second;
        conversationWorkspaceMap_.erase(workspaceIt);
    }
    return platform::Result<void>::success();
}

platform::Result<void> retoolapi::deleteUpstreamThread(
    const std::string&, const std::string&, const std::string&, const std::string&)
{
    // Retool does not expose a stable remote-thread deletion contract. Session
    // cleanup still clears this provider's local affinity through
    // eraseThreadContext(); callers that require remote reaping get an
    // explicit capability failure instead of a silent no-op.
    return platform::Result<void>::failure(platform::Error::notFound(
        "Retool does not expose upstream thread deletion"));
}
