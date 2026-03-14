#include "nexosapi.h"

#include <accountManager/accountManager.h>
#include <dbManager/account/accountBackupDbManager.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <regex>
#include <set>
#include <sstream>

using namespace drogon;

IMPLEMENT_RUNTIME(nexosapi, nexosapi);

namespace {

std::string trimCopy(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string jsonCompactString(const Json::Value& value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

std::string toLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool containsInsensitive(const std::string& text, const std::string& pattern)
{
    return toLowerCopy(text).find(toLowerCopy(pattern)) != std::string::npos;
}

std::string makeBoundary()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "----DrogonNexosBoundary" + std::to_string(now);
}

bool extractNumberAfterToken(const std::string& raw, const std::string& token, double& out)
{
    const auto pos = raw.find(token);
    if (pos == std::string::npos) {
        return false;
    }

    const auto comma = raw.find(',', pos + token.size());
    if (comma == std::string::npos) {
        return false;
    }

    std::size_t end = comma + 1;
    while (end < raw.size() && (std::isdigit(static_cast<unsigned char>(raw[end])) || raw[end] == '.')) {
        ++end;
    }

    const std::string num = raw.substr(comma + 1, end - comma - 1);
    if (num.empty()) {
        return false;
    }

    try {
        out = std::stod(num);
        return true;
    } catch (...) {
        return false;
    }
}

bool extractIntegerAfterToken(const std::string& raw, const std::string& token, int& out)
{
    double value = 0;
    if (!extractNumberAfterToken(raw, token, value)) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool isCycleMarkerString(const std::string& value)
{
    return value.size() >= 9 &&
           value.rfind("<cycle:", 0) == 0 &&
           value.back() == '>';
}

Json::Value sanitizeJsonValue(const Json::Value& value)
{
    if (value.isString()) {
        const std::string text = value.asString();
        if (isCycleMarkerString(text)) {
            return Json::nullValue;
        }
        return value;
    }

    if (value.isArray()) {
        Json::Value out(Json::arrayValue);
        for (const auto& item : value) {
            out.append(sanitizeJsonValue(item));
        }
        return out;
    }

    if (value.isObject()) {
        Json::Value out(Json::objectValue);
        for (const auto& key : value.getMemberNames()) {
            out[key] = sanitizeJsonValue(value[key]);
        }
        return out;
    }

    return value;
}

Json::Value resolveSpecialNegative(const Json::Value& value)
{
    if (!value.isInt() && !value.isInt64()) {
        return value;
    }
    const auto n = value.isInt64() ? value.asInt64() : value.asInt();
    if (n == -5 || n == -7) {
        return Json::nullValue;
    }
    return value;
}

Json::Value resolveInline(
    const Json::Value& value,
    const Json::Value& root,
    std::unordered_map<int, Json::Value>& memo,
    std::set<int>& inProgress
);

Json::Value resolveRef(
    int index,
    const Json::Value& root,
    std::unordered_map<int, Json::Value>& memo,
    std::set<int>& inProgress
)
{
    if (!root.isArray() || index < 0 || index >= static_cast<int>(root.size())) {
        return Json::Value(index);
    }

    auto memoIt = memo.find(index);
    if (memoIt != memo.end()) {
        return memoIt->second;
    }

    if (inProgress.count(index) > 0) {
        return Json::Value("<cycle:" + std::to_string(index) + ">");
    }

    inProgress.insert(index);
    Json::Value resolved = resolveInline(root[index], root, memo, inProgress);
    inProgress.erase(index);
    memo[index] = resolved;
    return resolved;
}

Json::Value resolveInline(
    const Json::Value& value,
    const Json::Value& root,
    std::unordered_map<int, Json::Value>& memo,
    std::set<int>& inProgress
)
{
    if (value.isNull() || value.isString() || value.isBool()) {
        return value;
    }

    if (value.isInt()) {
        const int n = value.asInt();
        if (n < 0) {
            return resolveSpecialNegative(value);
        }
        if (!root.isArray() || n >= static_cast<int>(root.size())) {
            return value;
        }
        return resolveRef(n, root, memo, inProgress);
    }

    if (value.isInt64()) {
        const auto n = value.asInt64();
        if (n < 0) {
            return resolveSpecialNegative(value);
        }
        if (!root.isArray() || n >= static_cast<Json::Int64>(root.size())) {
            return value;
        }
        return resolveRef(static_cast<int>(n), root, memo, inProgress);
    }

    if (value.isUInt()) {
        const unsigned int n = value.asUInt();
        if (!root.isArray() || n >= root.size()) {
            return value;
        }
        return resolveRef(static_cast<int>(n), root, memo, inProgress);
    }

    if (value.isUInt64()) {
        const auto n = value.asUInt64();
        if (!root.isArray() || n >= static_cast<Json::UInt64>(root.size())) {
            return value;
        }
        return resolveRef(static_cast<int>(n), root, memo, inProgress);
    }

    if (value.isDouble()) {
        return value;
    }

    if (value.isArray()) {
        Json::Value out(Json::arrayValue);
        for (const auto& item : value) {
            out.append(resolveInline(item, root, memo, inProgress));
        }
        return out;
    }

    if (value.isObject()) {
        Json::Value out(Json::objectValue);
        for (const auto& key : value.getMemberNames()) {
            std::string resolvedKey = key;
            if (key.size() > 1 && key[0] == '_' &&
                std::all_of(key.begin() + 1, key.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
                const int keyIndex = std::stoi(key.substr(1));
                const Json::Value keyValue = resolveRef(keyIndex, root, memo, inProgress);
                if (keyValue.isString()) {
                    resolvedKey = keyValue.asString();
                }
            }
            out[resolvedKey] = resolveInline(value[key], root, memo, inProgress);
        }
        return out;
    }

    return value;
}

Json::Value parseNexosSerializedData(const std::string& serializedText)
{
    Json::CharReaderBuilder builder;
    std::string errs;
    Json::Value root;
    std::istringstream iss(serializedText);
    if (!Json::parseFromStream(builder, iss, &root, &errs) || !root.isArray() || root.empty()) {
        return Json::Value();
    }

    std::unordered_map<int, Json::Value> memo;
    std::set<int> inProgress;
    return resolveRef(0, root, memo, inProgress);
}

Json::Value findFirstKeyDeep(const Json::Value& value, const std::string& targetKey)
{
    if (!value.isObject() && !value.isArray()) {
        return Json::Value();
    }

    if (value.isObject()) {
        if (value.isMember(targetKey) && value[targetKey].isArray()) {
            return value[targetKey];
        }
        for (const auto& key : value.getMemberNames()) {
            Json::Value found = findFirstKeyDeep(value[key], targetKey);
            if (!found.isNull()) {
                return found;
            }
        }
        if (value.isMember(targetKey)) {
            return value[targetKey];
        }
    } else {
        for (const auto& item : value) {
            Json::Value found = findFirstKeyDeep(item, targetKey);
            if (!found.isNull()) {
                return found;
            }
        }
    }

    return Json::Value();
}

std::string mapRuntimeModelAlias(const Json::Value& modelInfo)
{
    const std::string baseModelName = toLowerCopy(modelInfo.get("base_model_name", "").asString());
    const std::string customName = toLowerCopy(modelInfo.get("custom_name", "").asString());

    const std::vector<std::pair<std::string, std::vector<std::string>>> matchers = {
        {"claude-haiku-4-5", {"claude-haiku-4-5"}},
        {"claude-opus-4-5", {"claude-opus-4-5"}},
        {"claude-opus-4-6", {"claude-opus-4-6"}},
        {"claude-sonnet-4-5", {"claude-sonnet-4-5"}},
        {"claude-sonnet-4-6", {"claude-sonnet-4-6"}},
        {"gemini-2-5-flash", {"gemini-2.5-flash", "gemini-2-5-flash"}},
        {"gemini-2-5-pro", {"gemini-2.5-pro", "gemini-2-5-pro"}},
        {"gemini-3-flash-preview", {"gemini-3-flash-preview"}},
        {"gemini-3-pro-preview", {"gemini-3-pro-preview"}},
        {"gemini-3-1-pro-preview", {"gemini-3.1-pro-preview", "gemini-3-1-pro-preview"}},
        {"gpt-5", {"gpt-5-eu", "gpt-5"}},
        {"gpt-5-1", {"gpt-5.1", "gpt-5-1"}},
        {"gpt-5-2", {"gpt-5.2", "gpt-5-2"}},
        {"grok-4-fast", {"grok-4-fast-non-reasoning", "grok 4 fast"}},
        {"grok-4-fast-reasoning", {"grok-4-fast-reasoning", "grok 4 fast reasoning"}},
        {"grok-4-1-fast", {"grok-4-1-fast-non-reasoning", "grok 4.1 fast"}},
        {"grok-4-1-fast-reasoning", {"grok-4-1-fast-reasoning", "grok 4.1 fast reasoning"}},
        {"grok-code-fast-1", {"grok-code-fast-1", "grok code fast"}},
        {"mistral-large-3", {"mistral-large-2512", "mistral large 3"}},
        {"mistral-medium-3", {"mistral-medium-2505", "mistral medium 3"}},
        {"mistral-medium-3-1", {"mistral-medium-2508", "mistral medium 3.1"}}
    };

    for (const auto& [alias, patterns] : matchers) {
        for (const auto& pattern : patterns) {
            if (baseModelName.find(pattern) != std::string::npos ||
                customName.find(pattern) != std::string::npos) {
                return alias;
            }
        }
    }

    return "";
}

std::string getOwnedByForAlias(const std::string& alias, const Json::Value& modelInfo)
{
    if (alias.find("claude-") == 0) return "anthropic";
    if (alias.find("gemini-") == 0) return "google";
    if (alias.find("gpt-") == 0) return "openai";
    if (alias.find("grok-") == 0) return "xai";
    if (alias.find("mistral-") == 0) return "mistral";

    const Json::Value provider = modelInfo["provider"];
    const std::string providerName = toLowerCopy(provider.get("alias", provider.get("name", "")).asString());
    if (providerName.find("anthropic") != std::string::npos) return "anthropic";
    if (providerName.find("google") != std::string::npos) return "google";
    if (providerName.find("openai") != std::string::npos) return "openai";
    if (providerName.find("xai") != std::string::npos || providerName.find("x-ai") != std::string::npos) return "xai";
    if (providerName.find("mistral") != std::string::npos) return "mistral";
    return "nexos";
}

bool isUsableNexosAccount(const std::shared_ptr<Accountinfo_st>& account)
{
    return account &&
           account->tokenStatus &&
           account->accountStatus &&
           !account->authToken.empty() &&
           account->status != AccountStatus::DISABLED;
}

}  // namespace

nexosapi::nexosapi() = default;
nexosapi::~nexosapi() = default;

void* nexosapi::createApi()
{
    auto* api = new nexosapi();
    api->init();
    return api;
}

void nexosapi::init()
{
    const auto& customConfig = app().getCustomConfig();
    const Json::Value emptyObject(Json::objectValue);
    const auto& providers =
        customConfig.isMember("providers") && customConfig["providers"].isObject()
            ? customConfig["providers"]
            : emptyObject;
    const auto& nexos =
        providers.isMember("nexos") && providers["nexos"].isObject()
            ? providers["nexos"]
            : emptyObject;

    baseUrl_ = trimCopy(nexos.get("base_url", "https://workspace.nexos.ai").asString());
    if (baseUrl_.empty()) {
        baseUrl_ = "https://workspace.nexos.ai";
    }

    modelListOpenAiFormat_ = Json::objectValue;
    modelListOpenAiFormat_["object"] = "list";
    modelListOpenAiFormat_["data"] = Json::arrayValue;

    LOG_INFO << "[nexosapi] 初始化完成，baseUrl=" << baseUrl_
             << "，cookies/模型均改为运行时从账号管理与 chat.data 获取";
}

std::shared_ptr<Accountinfo_st> nexosapi::selectAccount(const session_st& session, bool& reuseExistingChat)
{
    return selectAccount(session, reuseExistingChat, {});
}

std::shared_ptr<Accountinfo_st> nexosapi::selectAccount(
    const session_st& session,
    bool& reuseExistingChat,
    const std::set<std::string>& excludedUserNames
)
{
    reuseExistingChat = false;
    const std::string providerKey = !session.provider.prevProviderKey.empty()
        ? session.provider.prevProviderKey
        : session.state.conversationId;

    std::string preferredUserName;
    {
        std::lock_guard<std::mutex> lock(chatMutex_);
        auto it = chatMap_.find(providerKey);
        if (it != chatMap_.end() && !it->second.accountUserName.empty()) {
            preferredUserName = it->second.accountUserName;
            reuseExistingChat = !it->second.chatId.empty();
        }
    }

    std::shared_ptr<Accountinfo_st> account;
    if (!preferredUserName.empty() && excludedUserNames.count(preferredUserName) == 0) {
        AccountManager::getInstance().getAccountByUserName("nexosapi", preferredUserName, account);
        if (isUsableNexosAccount(account)) {
            return account;
        }
        reuseExistingChat = false;
    }

    const auto accountList = AccountManager::getInstance().getAccountList();
    auto apiIt = accountList.find("nexosapi");
    if (apiIt == accountList.end()) {
        return nullptr;
    }

    std::shared_ptr<Accountinfo_st> selected;
    for (const auto& [userName, current] : apiIt->second) {
        if (excludedUserNames.count(userName) > 0 || !isUsableNexosAccount(current)) {
            continue;
        }
        if (!selected || current->useCount < selected->useCount) {
            selected = current;
        }
    }

    if (!selected) {
        return nullptr;
    }

    AccountManager::getInstance().getAccountByUserName("nexosapi", selected->userName, account);
    return isUsableNexosAccount(account) ? account : nullptr;
}

nexosapi::ChatDataPayload nexosapi::fetchChatDataPayload(const std::string& cookies) const
{
    ChatDataPayload payload;
    if (cookies.empty()) {
        return payload;
    }

    auto client = HttpClient::newHttpClient(baseUrl_);
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(Get);
    request->setPath("/chat.data");
    request->addHeader("accept", "*/*");
    request->addHeader("accept-language", "zh-CN,zh;q=0.9,en;q=0.8");
    request->addHeader("cache-control", "no-cache");
    request->addHeader("referer", baseUrl_ + "/");
    request->addHeader("user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36");
    request->addHeader("cookie", cookies);

    auto [result, response] = client->sendRequest(request);
    if (result != ReqResult::Ok || !response || response->statusCode() != k200OK) {
        return payload;
    }

    payload.raw = std::string(response->getBody());
    payload.decoded = parseNexosSerializedData(payload.raw);
    return payload;
}

std::shared_ptr<Accountinfo_st> nexosapi::selectAnyAccount()
{
    std::shared_ptr<Accountinfo_st> account;
    AccountManager::getInstance().getAccount("nexosapi", account, "");
    if (account && account->tokenStatus && !account->authToken.empty() && account->accountStatus) {
        return account;
    }
    return nullptr;
}

std::shared_ptr<Accountinfo_st> nexosapi::selectAccountByUserName(const std::string& userName)
{
    if (userName.empty()) {
        return nullptr;
    }

    std::shared_ptr<Accountinfo_st> account;
    AccountManager::getInstance().getAccountByUserName("nexosapi", userName, account);
    if (account && account->tokenStatus && !account->authToken.empty() && account->accountStatus) {
        return account;
    }
    return nullptr;
}

nexosapi::RuntimeModelData nexosapi::fetchRuntimeModelData(const std::string& cookies)
{
    RuntimeModelData runtimeData;
    runtimeData.models["object"] = "list";
    runtimeData.models["data"] = Json::arrayValue;

    if (cookies.empty()) {
        return runtimeData;
    }

    const auto payload = fetchChatDataPayload(cookies);
    const Json::Value userModels = findFirstKeyDeep(payload.decoded, "userModels");
    const std::string decodedPreview = jsonCompactString(payload.decoded);
    LOG_INFO << "[nexosapi] 运行时模型解析: body_size=" << payload.raw.size()
             << ", decoded_is_null=" << payload.decoded.isNull()
             << ", userModels_is_array=" << userModels.isArray()
             << ", userModels_size=" << (userModels.isArray() ? static_cast<int>(userModels.size()) : 0)
             << ", userModels_type=" << userModels.type()
             << ", userModels_preview=" << jsonCompactString(userModels).substr(0, 160)
             << ", decoded_preview=" << decodedPreview.substr(0, 200);

    std::set<std::string> seenAliases;
    if (userModels.isArray()) {
        for (const auto& entry : userModels) {
            if (!entry.isObject() || !entry.isMember("model")) {
                continue;
            }

            const Json::Value& modelInfo = entry["model"];
            const std::string alias = mapRuntimeModelAlias(modelInfo);
            if (alias.empty()) {
                continue;
            }

            const std::string handlerId = entry.get("id", "").asString();
            if (!handlerId.empty()) {
                runtimeData.mapping[alias] = handlerId;
            }

            if (seenAliases.insert(alias).second) {
                Json::Value model(Json::objectValue);
                model["id"] = alias;
                model["object"] = "model";
                model["created"] = static_cast<Json::Int64>(1677610602);
                model["owned_by"] = getOwnedByForAlias(alias, modelInfo);
                runtimeData.models["data"].append(model);
            }
        }
    }

    if (runtimeData.mapping.find("claude-opus-4-6") != runtimeData.mapping.end()) {
        runtimeData.defaultAlias = "claude-opus-4-6";
    } else if (!runtimeData.mapping.empty()) {
        runtimeData.defaultAlias = runtimeData.mapping.begin()->first;
    }

    if (!runtimeData.defaultAlias.empty()) {
        runtimeData.defaultHandlerId = runtimeData.mapping[runtimeData.defaultAlias];
        runtimeData.mapping["nexos-chat"] = runtimeData.defaultHandlerId;

        Json::Value model(Json::objectValue);
        model["id"] = "nexos-chat";
        model["object"] = "model";
        model["created"] = static_cast<Json::Int64>(1677610602);
        model["owned_by"] = "nexos";
        runtimeData.models["data"].append(model);
    }

    LOG_INFO << "[nexosapi] 运行时模型映射完成: aliases=" << runtimeData.mapping.size()
             << ", defaultAlias=" << runtimeData.defaultAlias;

    return runtimeData;
}

Json::Value nexosapi::buildModelList(const RuntimeModelData& runtimeModels)
{
    std::lock_guard<std::mutex> lock(modelMutex_);
    modelListOpenAiFormat_ = runtimeModels.models;
    if (!modelListOpenAiFormat_.isObject()) {
        modelListOpenAiFormat_ = Json::objectValue;
        modelListOpenAiFormat_["object"] = "list";
        modelListOpenAiFormat_["data"] = Json::arrayValue;
    }

    ModelInfoMap.clear();
    if (modelListOpenAiFormat_.isMember("data") && modelListOpenAiFormat_["data"].isArray()) {
        for (const auto& item : modelListOpenAiFormat_["data"]) {
            modelInfo info;
            info.modelName = item.get("id", "").asString();
            info.status = true;
            if (!info.modelName.empty()) {
                ModelInfoMap[info.modelName] = info;
            }
        }
    }

    return modelListOpenAiFormat_;
}

void nexosapi::checkAlivableTokens()
{
    std::shared_ptr<Accountinfo_st> account;
    AccountManager::getInstance().getAccount("nexosapi", account, "");
    if (!account || account->authToken.empty()) {
        LOG_WARN << "[nexosapi] 账号池中未找到可用 cookies";
    }
}

void nexosapi::checkModels()
{
}

Json::Value nexosapi::getModels()
{
    std::shared_ptr<Accountinfo_st> account;
    AccountManager::getInstance().getAccount("nexosapi", account, "");
    if (!account || !account->tokenStatus || account->authToken.empty()) {
        std::lock_guard<std::mutex> lock(modelMutex_);
        return modelListOpenAiFormat_;
    }

    const auto runtimeModels = fetchRuntimeModelData(account->authToken);
    return buildModelList(runtimeModels);
}

Json::Value nexosapi::buildQuotaResponse(const std::shared_ptr<Accountinfo_st>& account, const ChatDataPayload& payload) const
{
    Json::Value result(Json::objectValue);
    result["provider"] = "nexosapi";
    result["available"] = false;

    if (!account) {
        result["error"] = "No available nexos account";
        return result;
    }

    result["account"]["userName"] = account->userName;
    result["account"]["accountType"] = account->accountType;

    if (payload.raw.empty()) {
        result["error"] = "Failed to fetch chat.data";
        return result;
    }

    const Json::Value subscription =
        payload.decoded["domains/auth/routes/LoggedInLayout"]["data"].isObject()
            ? payload.decoded["domains/auth/routes/LoggedInLayout"]["data"]["subscription"]
            : Json::Value();

    Json::Value quota(Json::objectValue);
    if (subscription.isObject()) {
        if (subscription.isMember("status")) quota["status"] = sanitizeJsonValue(subscription["status"]);
        if (subscription.isMember("subscription_type")) quota["subscription_type"] = sanitizeJsonValue(subscription["subscription_type"]);
        if (subscription.isMember("start_at")) quota["start_at"] = sanitizeJsonValue(subscription["start_at"]);
        if (subscription.isMember("end_at")) quota["end_at"] = sanitizeJsonValue(subscription["end_at"]);
        if (subscription.isMember("enabled")) quota["enabled"] = sanitizeJsonValue(subscription["enabled"]);
        if (subscription.isMember("auto_renew")) quota["auto_renew"] = sanitizeJsonValue(subscription["auto_renew"]);
        if (subscription.isMember("budget_used") &&
            (subscription["budget_used"].isDouble() || subscription["budget_used"].isInt() || subscription["budget_used"].isUInt())) {
            quota["budget_used"] = subscription["budget_used"];
        }
    }

    double budgetUsed = 0;
    if (extractNumberAfterToken(payload.raw, "\"budget_used\"", budgetUsed)) {
        quota["budget_used_raw"] = budgetUsed;
    }

    int seatsUsed = 0;
    if (extractIntegerAfterToken(payload.raw, "\"seats_used\"", seatsUsed)) {
        quota["seats_used_raw"] = seatsUsed;
    }

    int userLimit = 0;
    if (extractIntegerAfterToken(payload.raw, "\"user_limit\"", userLimit)) {
        quota["user_limit_raw"] = userLimit;
    }

    if (payload.raw.find("\"trial_active\"") != std::string::npos) {
        quota["trial_active_detected"] = true;
    }

    if (payload.raw.find("\"type\",\"trial\"") != std::string::npos) {
        quota["trial_type_detected"] = true;
    }

    const Json::Value whoamiPayload = fetchWhoamiPayload(account->authToken);
    const std::string email = extractEmailFromWhoami(whoamiPayload);
    if (!email.empty()) {
        result["account"]["email"] = email;
    }

    result["available"] = !quota.empty();
    result["quota"] = sanitizeJsonValue(quota);
    return result;
}

Json::Value nexosapi::getAccountQuota(const std::string& userName)
{
    auto account = userName.empty() ? selectAnyAccount() : selectAccountByUserName(userName);
    if (!account) {
        Json::Value result(Json::objectValue);
        result["provider"] = "nexosapi";
        result["available"] = false;
        result["error"] = userName.empty()
            ? "No available nexos account"
            : ("Nexos account not found or unavailable: " + userName);
        return result;
    }

    const auto payload = fetchChatDataPayload(account->authToken);
    return buildQuotaResponse(account, payload);
}

provider::ProviderError nexosapi::classifyHttpError(int httpStatus, const std::string& message) const
{
    if (httpStatus == 401 || httpStatus == 403) {
        provider::ProviderError err = provider::ProviderError::auth(message);
        err.httpStatusCode = httpStatus;
        return err;
    }
    if (httpStatus == 408 || httpStatus == 504) {
        provider::ProviderError err = provider::ProviderError::timeout(message);
        err.httpStatusCode = httpStatus;
        return err;
    }
    if (httpStatus == 429) {
        provider::ProviderError err = provider::ProviderError::rateLimited(message);
        err.httpStatusCode = httpStatus;
        return err;
    }
    if (httpStatus >= 500) {
        provider::ProviderError err = provider::ProviderError::internal(message);
        err.httpStatusCode = httpStatus;
        return err;
    }

    provider::ProviderError err;
    err.code = provider::ProviderErrorCode::InvalidRequest;
    err.message = message;
    err.httpStatusCode = httpStatus > 0 ? httpStatus : 400;
    return err;
}

std::string nexosapi::extractChatIdFromChatData(const std::string& body) const
{
    static const std::vector<std::regex> patterns = {
        std::regex(R"(/chat/([a-f0-9-]{36}))", std::regex::icase),
        std::regex("\"chatId\",\"([a-f0-9-]{36})\"", std::regex::icase),
        std::regex("\"chatId\"\\s*:\\s*\"([a-f0-9-]{36})\"", std::regex::icase)
    };

    std::smatch match;
    for (const auto& pattern : patterns) {
        if (std::regex_search(body, match, pattern) && match.size() > 1) {
            return match[1].str();
        }
    }
    return "";
}

bool nexosapi::isBudgetExceededResponse(int httpStatus, const std::string& body) const
{
    if (httpStatus != 402 || body.empty()) {
        return false;
    }

    return containsInsensitive(body, "budget 5 has been reached for company") ||
           containsInsensitive(body, "trial_budget_exceeded") ||
           containsInsensitive(body, "\"code\":110001");
}

void nexosapi::markAccountBudgetExceeded(const std::shared_ptr<Accountinfo_st>& account) const
{
    if (!account) {
        return;
    }

    account->tokenStatus = false;
    account->accountStatus = false;
    account->accountType = "trial_budget_exceeded";
    account->status = AccountStatus::DISABLED;

    const bool backedUp = AccountBackupDbManager::getInstance()->backupAccount(
        *account,
        "trial_budget_exceeded"
    );

    if (!backedUp) {
        const bool memoryUpdated = AccountManager::getInstance().updateAccount(*account);
        const bool dbUpdated = AccountDbManager::getInstance()->updateAccount(*account);
        LOG_ERROR << "[nexosapi] 账号预算已耗尽，但备份失败，已仅标记不可用: userName="
                  << account->userName
                  << ", memoryUpdated=" << memoryUpdated
                  << ", dbUpdated=" << dbUpdated;
        return;
    }

    const bool dbDeleted = AccountDbManager::getInstance()->deleteAccount(account->apiName, account->userName);
    if (!dbDeleted) {
        const bool memoryUpdated = AccountManager::getInstance().updateAccount(*account);
        const bool dbUpdated = AccountDbManager::getInstance()->updateAccount(*account);
        LOG_ERROR << "[nexosapi] 账号已备份，但主库删除失败，已回退为禁用状态保留: userName="
                  << account->userName
                  << ", memoryUpdated=" << memoryUpdated
                  << ", dbUpdated=" << dbUpdated;
        return;
    }

    const bool memoryDeleted = AccountManager::getInstance().deleteAccountbyPost(account->apiName, account->userName);

    LOG_WARN << "[nexosapi] 账号预算已耗尽，已迁移到备份库并从主库删除: userName="
             << account->userName
             << ", dbDeleted=" << dbDeleted
             << ", memoryDeleted=" << memoryDeleted;
}

std::string nexosapi::createChatId(const std::string& cookies) const
{
    if (cookies.empty()) {
        return "";
    }

    auto client = HttpClient::newHttpClient(baseUrl_);
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(Get);
    request->setPath("/chat.data");
    request->addHeader("accept", "*/*");
    request->addHeader("accept-language", "zh-CN,zh;q=0.9,en;q=0.8");
    request->addHeader("cache-control", "no-cache");
    request->addHeader("referer", baseUrl_ + "/");
    request->addHeader("user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36");
    request->addHeader("cookie", cookies);

    auto [result, response] = client->sendRequest(request);
    if (result != ReqResult::Ok || !response || response->statusCode() != k200OK) {
        return "";
    }

    return extractChatIdFromChatData(std::string(response->getBody()));
}

std::string nexosapi::ensureChatId(const session_st& session, const std::shared_ptr<Accountinfo_st>& account, bool reuseExistingChat)
{
    const std::string key = !session.provider.prevProviderKey.empty()
        ? session.provider.prevProviderKey
        : session.state.conversationId;

    if (reuseExistingChat) {
        std::lock_guard<std::mutex> lock(chatMutex_);
        auto it = chatMap_.find(key);
        if (it != chatMap_.end() &&
            !it->second.chatId.empty() &&
            it->second.accountUserName == account->userName) {
            return it->second.chatId;
        }
    }

    const std::string chatId = createChatId(account->authToken);
    if (chatId.empty()) {
        return "";
    }

    std::lock_guard<std::mutex> lock(chatMutex_);
    chatMap_[key] = ChatContext{chatId, account->userName};
    return chatId;
}

std::string nexosapi::resolveHandlerId(const RuntimeModelData& runtimeModels, const std::string& requestedModel) const
{
    const std::string targetModel = requestedModel.empty() ? "nexos-chat" : requestedModel;

    auto it = runtimeModels.mapping.find(targetModel);
    if (it != runtimeModels.mapping.end() && !it->second.empty()) {
        return it->second;
    }

    auto fallback = runtimeModels.mapping.find("nexos-chat");
    if (fallback != runtimeModels.mapping.end() && !fallback->second.empty()) {
        return fallback->second;
    }

    if (!runtimeModels.defaultHandlerId.empty()) {
        return runtimeModels.defaultHandlerId;
    }

    return "";
}

std::string nexosapi::fetchLastMessageId(const std::string& chatId, const std::string& cookies) const
{
    if (chatId.empty() || cookies.empty()) {
        return "";
    }

    auto client = HttpClient::newHttpClient(baseUrl_);
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(Get);
    request->setPath("/api/chat/" + chatId + "/history?offset=0");
    request->addHeader("accept", "*/*");
    request->addHeader("accept-language", "zh-CN,zh;q=0.9,en;q=0.8");
    request->addHeader("cache-control", "no-cache");
    request->addHeader("content-type", "application/json");
    request->addHeader("referer", baseUrl_ + "/chat/" + chatId);
    request->addHeader("user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36");
    request->addHeader("cookie", cookies);

    auto [result, response] = client->sendRequest(request);
    if (result != ReqResult::Ok || !response || response->statusCode() != k200OK) {
        return "";
    }

    auto json = response->getJsonObject();
    if (!json || !json->isMember("items") || !(*json)["items"].isArray() || (*json)["items"].empty()) {
        return "";
    }

    const auto& item = (*json)["items"][0];
    if (item.isObject() && item.isMember("id") && item["id"].isString()) {
        return item["id"].asString();
    }

    return "";
}

std::string nexosapi::buildMultipartBody(
    const std::string& boundary,
    const std::string& chatId,
    const Json::Value& data
) const
{
    std::ostringstream body;
    const auto dataJson = jsonCompactString(data);

    body << "--" << boundary << "\r\n"
         << "Content-Disposition: form-data; name=\"action\"\r\n\r\n"
         << "chat_completion\r\n";

    body << "--" << boundary << "\r\n"
         << "Content-Disposition: form-data; name=\"chatId\"\r\n\r\n"
         << chatId << "\r\n";

    body << "--" << boundary << "\r\n"
         << "Content-Disposition: form-data; name=\"data\"\r\n\r\n"
         << dataJson << "\r\n";

    body << "--" << boundary << "--\r\n";
    return body.str();
}

std::string nexosapi::sendChatRequest(
    const std::string& chatId,
    const std::string& handlerId,
    const std::string& userText,
    const std::string& lastMessageId,
    const std::string& cookies,
    int& httpStatus
) const
{
    Json::Value data(Json::objectValue);
    data["handler"]["id"] = handlerId;
    data["handler"]["type"] = "model";
    data["handler"]["fallbacks"] = true;
    data["user_message"]["text"] = userText;
    data["user_message"]["client_metadata"] = Json::objectValue;
    data["user_message"]["files"] = Json::arrayValue;
    data["advanced_parameters"] = Json::objectValue;
    data["functionalityHeader"] = "chat";
    data["tools"]["web_search"]["enabled"] = true;
    data["tools"]["deep_research"]["enabled"] = false;
    data["tools"]["code_interpreter"]["enabled"] = true;
    data["enabled_integrations"] = Json::arrayValue;
    data["chat"] = Json::objectValue;
    if (!lastMessageId.empty()) {
        data["chat"]["last_message_id"] = lastMessageId;
    }

    const std::string boundary = makeBoundary();
    const std::string body = buildMultipartBody(boundary, chatId, data);

    auto client = HttpClient::newHttpClient(baseUrl_);
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(Post);
    request->setPath("/api/chat/" + chatId);
    request->setContentTypeString("multipart/form-data; boundary=" + boundary);
    request->setBody(body);
    request->addHeader("accept", "*/*");
    request->addHeader("accept-language", "zh-CN,zh;q=0.9,en;q=0.8");
    request->addHeader("cache-control", "no-cache");
    request->addHeader("origin", baseUrl_);
    request->addHeader("referer", baseUrl_ + "/chat/" + chatId);
    request->addHeader("sec-fetch-dest", "empty");
    request->addHeader("sec-fetch-mode", "cors");
    request->addHeader("sec-fetch-site", "same-origin");
    request->addHeader("user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36");
    request->addHeader("cookie", cookies);

    auto [result, response] = client->sendRequest(request);
    if (result != ReqResult::Ok || !response) {
        httpStatus = 0;
        return "";
    }

    httpStatus = static_cast<int>(response->statusCode());
    return std::string(response->getBody());
}

std::string nexosapi::extractTextFromSsePayload(const std::string& payload) const
{
    std::stringstream ss(payload);
    std::string line;
    std::string fullText;

    while (std::getline(ss, line)) {
        if (line.rfind("data: ", 0) != 0 || line.find("[DONE]") != std::string::npos) {
            continue;
        }

        Json::CharReaderBuilder builder;
        std::string errs;
        Json::Value json;
        std::istringstream input(line.substr(6));
        if (!Json::parseFromStream(builder, input, &json, &errs)) {
            continue;
        }

        if (json.get("content_type", "").asString() == "text" &&
            json.isMember("content") &&
            json["content"].isObject() &&
            json["content"].isMember("text") &&
            json["content"]["text"].isString()) {
            fullText += json["content"]["text"].asString();
        }
    }

    return fullText;
}

Json::Value nexosapi::fetchWhoamiPayload(const std::string& cookies) const
{
    if (cookies.empty()) {
        return Json::Value();
    }

    auto client = HttpClient::newHttpClient(baseUrl_);
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(Get);
    request->setPath("/oryBridge/.ory/sessions/whoami");
    request->addHeader("accept", "application/json");
    request->addHeader("user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36");
    request->addHeader("cookie", cookies);

    auto [result, response] = client->sendRequest(request);
    if (result != ReqResult::Ok || !response || response->statusCode() != k200OK) {
        return Json::Value();
    }

    auto json = response->getJsonObject();
    if (!json || !json->isObject()) {
        return Json::Value();
    }
    return *json;
}

std::string nexosapi::extractEmailFromWhoami(const Json::Value& whoamiPayload) const
{
    if (!whoamiPayload.isObject()) {
        return "";
    }

    const auto& identity = whoamiPayload["identity"];
    if (!identity.isObject()) {
        return "";
    }

    const auto& traits = identity["traits"];
    if (traits.isObject() && traits.isMember("email") && traits["email"].isString()) {
        return traits["email"].asString();
    }

    const auto& addresses = identity["verifiable_addresses"];
    if (addresses.isArray()) {
        for (const auto& address : addresses) {
            if (address.isObject() && address.isMember("value") && address["value"].isString()) {
                return address["value"].asString();
            }
        }
    }

    return "";
}

std::string nexosapi::buildUserPrompt(const session_st& session, bool useExistingChat) const
{
    if (useExistingChat) {
        return session.request.message;
    }

    std::ostringstream prompt;

    if (!session.request.systemPrompt.empty()) {
        prompt << session.request.systemPrompt;
    }

    if (!session.provider.messageContext.empty()) {
        if (prompt.tellp() > 0) {
            prompt << "\n\n";
        }
        prompt << "接下来是 OpenAI 风格的历史消息，请继续保持上下文一致：\n";
        prompt << jsonCompactString(session.provider.messageContext);
    }

    if (!session.request.message.empty()) {
        if (prompt.tellp() > 0) {
            prompt << "\n\n";
        }
        prompt << "用户当前的问题是：\n" << session.request.message;
    }

    return prompt.str();
}

provider::ProviderResult nexosapi::requestChatCompletion(session_st& session)
{
    std::set<std::string> excludedUserNames;
    std::string lastFailureMessage = "No available nexos account/cookies in account manager";
    int lastHttpStatus = 0;

    while (true) {
        bool reuseExistingChat = false;
        auto account = selectAccount(session, reuseExistingChat, excludedUserNames);
        if (!account) {
            if (lastHttpStatus > 0) {
                return provider::ProviderResult::fail(classifyHttpError(lastHttpStatus, lastFailureMessage));
            }
            return provider::ProviderResult::fail(
                provider::ProviderError::auth("No available nexos account/cookies in account manager")
            );
        }

        excludedUserNames.insert(account->userName);

        const auto runtimeModels = fetchRuntimeModelData(account->authToken);
        buildModelList(runtimeModels);

        const std::string handlerId = resolveHandlerId(runtimeModels, session.request.model);
        if (handlerId.empty()) {
            return provider::ProviderResult::fail(
                provider::ProviderError::internal("Failed to resolve nexos model handler from runtime model list")
            );
        }

        const std::string chatId = ensureChatId(session, account, reuseExistingChat);
        if (chatId.empty()) {
            lastFailureMessage = "Failed to create nexos chat";
            lastHttpStatus = 0;
            continue;
        }

        const std::string lastMessageId = reuseExistingChat
            ? fetchLastMessageId(chatId, account->authToken)
            : "";
        const std::string prompt = buildUserPrompt(session, reuseExistingChat);

        int httpStatus = 0;
        const std::string raw = sendChatRequest(
            chatId,
            handlerId,
            prompt,
            lastMessageId,
            account->authToken,
            httpStatus
        );

        if (httpStatus != 200) {
            std::string message = "Nexos upstream returned error";
            if (!raw.empty()) {
                message += ": " + raw.substr(0, 500);
            }

            lastFailureMessage = message;
            lastHttpStatus = httpStatus;

            if (isBudgetExceededResponse(httpStatus, raw)) {
                markAccountBudgetExceeded(account);
                LOG_WARN << "[nexosapi] 检测到预算耗尽，切换账号重试: userName=" << account->userName;
                continue;
            }

            return provider::ProviderResult::fail(classifyHttpError(httpStatus, message));
        }

        const std::string text = extractTextFromSsePayload(raw);
        if (text.empty()) {
            provider::ProviderError err = provider::ProviderError::internal("Nexos returned empty response");
            err.httpStatusCode = 502;
            return provider::ProviderResult::fail(err);
        }

        provider::ProviderResult result = provider::ProviderResult::success(text);
        result.rawResponse = raw;
        return result;
    }
}

provider::ProviderResult nexosapi::generate(session_st& session)
{
    return requestChatCompletion(session);
}

void nexosapi::afterResponseProcess(session_st&)
{
}

void nexosapi::eraseChatinfoMap(std::string conversationId)
{
    std::lock_guard<std::mutex> lock(chatMutex_);
    chatMap_.erase(conversationId);
}

void nexosapi::transferThreadContext(const std::string& oldId, const std::string& newId)
{
    if (oldId.empty() || newId.empty() || oldId == newId) {
        return;
    }

    std::lock_guard<std::mutex> lock(chatMutex_);
    auto it = chatMap_.find(oldId);
    if (it == chatMap_.end()) {
        return;
    }

    chatMap_[newId] = it->second;
    chatMap_.erase(it);
}
