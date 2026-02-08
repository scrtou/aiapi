#include "OpenAiProvider.h"
#include <drogon/drogon.h>
#include <apipoint/ProviderResult.h>
#include <apiManager/ApiManager.h>

using namespace drogon;

IMPLEMENT_RUNTIME(OpenAiProvider, OpenAiProvider);

OpenAiProvider::OpenAiProvider() = default;
OpenAiProvider::~OpenAiProvider() = default;

void* OpenAiProvider::createApi() {
    auto* api = new OpenAiProvider();
    api->init();
    return api;
}

void OpenAiProvider::init() {
    const auto& customConfig = app().getCustomConfig();
    if (customConfig.isMember("providers") && customConfig["providers"].isObject() &&
        customConfig["providers"].isMember("openai") && customConfig["providers"]["openai"].isObject()) {
        const auto& openai = customConfig["providers"]["openai"];
        apiKey_ = openai.get("api_key", "").asString();
        baseUrl_ = openai.get("base_url", "https://api.openai.com").asString();
        defaultModel_ = openai.get("default_model", "gpt-4o-mini").asString();
    } else {
        apiKey_.clear();
        baseUrl_ = "https://api.openai.com";
        defaultModel_ = "gpt-4o-mini";
    }

    checkModels();
}

provider::ProviderResult OpenAiProvider::requestChatCompletions(session_st& session) {
    if (apiKey_.empty()) {
        return provider::ProviderResult::fail(provider::ProviderError::auth("OpenAI API key not configured"));
    }

    auto client = HttpClient::newHttpClient(baseUrl_);
    if (!client) {
        return provider::ProviderResult::fail(provider::ProviderError::network("Failed to create HTTP client"));
    }

    auto req = HttpRequest::newHttpJsonRequest(buildChatRequest(session));
    req->setMethod(Post);
    req->setPath("/v1/chat/completions");
    req->addHeader("Authorization", "Bearer " + apiKey_);

    auto [result, resp] = client->sendRequest(req);
    if (result != ReqResult::Ok || !resp) {
        return provider::ProviderResult::fail(provider::ProviderError::network("OpenAI request failed"));
    }

    auto json = resp->getJsonObject();
    if (!json) {
        provider::ProviderError err = provider::ProviderError::internal("OpenAI response JSON parse failed");
        err.httpStatusCode = static_cast<int>(resp->statusCode());
        return provider::ProviderResult::fail(err);
    }

    if (resp->statusCode() != k200OK) {
        provider::ProviderError err;
        err.code = provider::ProviderErrorCode::Unknown;
        err.httpStatusCode = static_cast<int>(resp->statusCode());
        err.message = (*json).get("error", Json::Value(Json::objectValue)).get("message", "OpenAI API error").asString();
        return provider::ProviderResult::fail(err);
    }

    provider::ProviderResult out;
    out.statusCode = 200;

    const auto& choices = (*json)["choices"];
    if (choices.isArray() && !choices.empty()) {
        const auto& message = choices[0]["message"];
        if (message.isObject()) {
            out.text = message.get("content", "").asString();
            if (message.isMember("tool_calls") && message["tool_calls"].isArray()) {
                for (const auto& tc : message["tool_calls"]) {
                    provider::ToolCall call;
                    call.id = tc.get("id", "").asString();
                    if (tc.isMember("function") && tc["function"].isObject()) {
                        call.name = tc["function"].get("name", "").asString();
                        call.arguments = tc["function"].get("arguments", "{}").asString();
                    }
                    out.toolCalls.push_back(call);
                }
            }
        }
    }

    if ((*json).isMember("usage") && (*json)["usage"].isObject()) {
        provider::Usage usage;
        usage.inputTokens = (*json)["usage"].get("prompt_tokens", 0).asInt();
        usage.outputTokens = (*json)["usage"].get("completion_tokens", 0).asInt();
        usage.totalTokens = (*json)["usage"].get("total_tokens", 0).asInt();
        out.usage = usage;
    }

    out.error = provider::ProviderError::none();
    out.rawResponse = resp->getBody();
    return out;
}

Json::Value OpenAiProvider::buildChatRequest(const session_st& session) const {
    Json::Value body(Json::objectValue);
    body["model"] = session.request.model.empty() ? defaultModel_ : session.request.model;

    Json::Value messages(Json::arrayValue);

    if (!session.request.systemPrompt.empty()) {
        Json::Value systemMsg;
        systemMsg["role"] = "system";
        systemMsg["content"] = session.request.systemPrompt;
        messages.append(systemMsg);
    }

    for (const auto& m : session.provider.messageContext) {
        if (!m.isObject()) continue;
        Json::Value msg;
        msg["role"] = m.get("role", "user").asString();
        msg["content"] = m.get("content", "").asString();
        messages.append(msg);
    }

    if (!session.request.message.empty()) {
        Json::Value userMsg;
        userMsg["role"] = "user";
        userMsg["content"] = session.request.message;
        messages.append(userMsg);
    }

    body["messages"] = messages;

    if (!session.request.tools.isNull() && session.request.tools.isArray() && session.request.tools.size() > 0) {
        body["tools"] = session.request.tools;
        if (!session.request.toolChoice.empty()) {
            if (session.request.toolChoice.front() == '{') {
                Json::CharReaderBuilder rb;
                Json::Value toolChoiceObj;
                std::string errs;
                std::istringstream iss(session.request.toolChoice);
                if (Json::parseFromStream(rb, iss, &toolChoiceObj, &errs)) {
                    body["tool_choice"] = toolChoiceObj;
                }
            } else {
                body["tool_choice"] = session.request.toolChoice;
            }
        }
    }

    body["stream"] = false;
    return body;
}

void OpenAiProvider::postChatMessage(session_st& session) {
    const auto result = generate(session);
    session.response.message["message"] = result.text;
    session.response.message["statusCode"] = result.statusCode;
    if (result.error.hasError()) {
        session.response.message["error"] = result.error.message;
    }
    if (!result.toolCalls.empty()) {
        Json::Value toolCalls(Json::arrayValue);
        for (const auto& tc : result.toolCalls) {
            Json::Value item(Json::objectValue);
            item["id"] = tc.id;
            item["name"] = tc.name;
            item["arguments"] = tc.arguments;
            toolCalls.append(item);
        }
        session.response.message["tool_calls"] = toolCalls;
    }
}

provider::ProviderResult OpenAiProvider::generate(session_st& session) {
    return requestChatCompletions(session);
}

void OpenAiProvider::checkAlivableTokens() {
    if (apiKey_.empty()) {
        LOG_WARN << "[OpenAi上游] api_key 未配置，跳过 可用性检测";
    }
}

void OpenAiProvider::checkModels() {
    if (apiKey_.empty()) {
        return;
    }

    auto client = HttpClient::newHttpClient(baseUrl_);
    if (!client) return;

    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/v1/models");
    req->addHeader("Authorization", "Bearer " + apiKey_);

    auto [result, resp] = client->sendRequest(req);
    if (result != ReqResult::Ok || !resp || resp->statusCode() != k200OK) {
        return;
    }

    auto json = resp->getJsonObject();
    if (!json || !(*json).isMember("data") || !(*json)["data"].isArray()) {
        return;
    }

    std::lock_guard<std::mutex> lock(modelMutex_);
    modelList_ = (*json)["data"];

    modelListOpenAiFormat_ = Json::Value(Json::objectValue);
    modelListOpenAiFormat_["object"] = "list";
    modelListOpenAiFormat_["data"] = Json::Value(Json::arrayValue);
    for (const auto& item : modelList_) {
        Json::Value model;
        model["id"] = item.get("id", "").asString();
        model["object"] = "model";
        modelListOpenAiFormat_["data"].append(model);

        modelInfo info;
        info.modelName = item.get("id", "").asString();
        info.status = true;
        ModelInfoMap[info.modelName] = info;
    }
}

Json::Value OpenAiProvider::getModels() {
    std::lock_guard<std::mutex> lock(modelMutex_);
    return modelListOpenAiFormat_;
}

void OpenAiProvider::afterResponseProcess(session_st&) {
}

void OpenAiProvider::eraseChatinfoMap(std::string) {
}

void OpenAiProvider::transferThreadContext(const std::string&, const std::string&) {
}
