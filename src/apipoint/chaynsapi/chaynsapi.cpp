#include <drogon/drogon.h>
#include <chaynsapi.h>
#include <../../apiManager/Apicomn.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <sessionManager/continuity/HistoryReplayBudget.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utils/chaynsBrowserImpersonation.h>
IMPLEMENT_RUNTIME(chaynsapi,chaynsapi);
using namespace drogon;

namespace {

constexpr auto MODEL_CACHE_TTL = std::chrono::minutes(15);
constexpr auto MODEL_REFRESH_MIN_INTERVAL = std::chrono::seconds(30);

constexpr const char* CHAYNS_FREE_ORIGIN = "https://sidekick.ki";
constexpr const char* CHAYNS_FREE_REFERER = "https://sidekick.ki/";
constexpr const char* CHAYNS_PRO_ORIGIN = "https://mein.sidekick.ki";
constexpr const char* CHAYNS_PRO_REFERER = "https://mein.sidekick.ki/";

struct ChaynsRequestRoute
{
    bool isPro = false;
    int threadTypeId = 8;
    std::int64_t workspaceUacId = 0;
    std::string origin = CHAYNS_FREE_ORIGIN;
    std::string referer = CHAYNS_FREE_REFERER;
};

ChaynsRequestRoute requestRouteForAccount(const Accountinfo_st& account)
{
    ChaynsRequestRoute route;
    route.isPro = account.accountType == "pro";
    if (route.isPro) {
        route.threadTypeId = 9;
        route.workspaceUacId = account.workspaceUacId;
        route.origin = CHAYNS_PRO_ORIGIN;
        route.referer = CHAYNS_PRO_REFERER;
    }
    return route;
}

void applyChaynsRouteHeaders(const HttpRequestPtr& request,
                             const Accountinfo_st& account,
                             const ChaynsRequestRoute& route)
{
    chayns_browser::applyBrowserHeadersForAccount(
        request,
        account.userName,
        account.personId,
        route.origin,
        route.referer);
}

std::mutex g_chaynsAccountGateMapMutex;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> g_chaynsAccountGates;

std::shared_ptr<std::mutex> accountExecutionGate(const std::string& accountUserName)
{
    std::lock_guard<std::mutex> lock(g_chaynsAccountGateMapMutex);
    auto& gate = g_chaynsAccountGates[accountUserName];
    if (!gate) {
        gate = std::make_shared<std::mutex>();
    }
    return gate;
}

bool isUsableChaynsAccount(const std::shared_ptr<Accountinfo_st>& account, bool requiresPro)
{
    if (!account || !account->tokenStatus || !account->accountStatus ||
        account->status != AccountStatus::ACTIVE || account->authToken.empty()) {
        return false;
    }
    if (requiresPro) {
        return account->accountType == "pro" && account->workspaceUacId > 0;
    }
    return account->accountType == "free";
}

bool postFailureMayHaveBeenAccepted(HttpStatusCode status)
{
    const int code = static_cast<int>(status);
    return code == 408 || code >= 500;
}

void setChaynsSessionError(session_st& session,
                           int statusCode,
                           const std::string& code,
                           const std::string& message)
{
    session.response.message["error"] = message;
    session.response.message["errorCode"] = code;
    session.response.message["statusCode"] = statusCode;
}

// Upstream bodies may carry conversation content, identifiers, cookies, or
// error details.  Logging only transport metadata keeps failure diagnostics
// useful without duplicating that sensitive data into aiapi.log.
std::string summarizeUpstreamResponse(const HttpResponsePtr& response)
{
    if (!response) {
        return "responsePresent=false";
    }
    return "responsePresent=true, status=" +
           std::to_string(static_cast<int>(response->statusCode())) +
           ", bodySize=" + std::to_string(response->getBody().size());
}

}  // namespace

chaynsapi::chaynsapi()
{
   
}

chaynsapi::~chaynsapi()
{
}

void chaynsapi::init()
{
    chayns_browser::reloadConfigFromDrogon();

    loadModels(true);

    // 从配置 custom_config.upstream_error_texts 加载上游错误文本列表
    auto& customConfig = drogon::app().getCustomConfig();
    if (customConfig.isMember("upstream_error_texts") && customConfig["upstream_error_texts"].isArray()) {
        for (const auto& item : customConfig["upstream_error_texts"]) {
            if (item.isString()) {
                m_upstreamErrorTexts.push_back(item.asString());
            }
        }
        LOG_INFO << "[chaynsAPI] 已从配置加载" << m_upstreamErrorTexts.size() << " 条上游错误文本";
    } else {
        LOG_WARN << "[chaynsAPI] 配置中未找到 upstream_error_texts，上游错误文本匹配将不可用";
    }
}


std::string chaynsapi::uploadImageToService(const ImageInfo& image,
                                            const std::string& personId,
                                            const std::string& authToken,
                                            const std::string& accountUserName,
                                            const std::string& origin,
                                            const std::string& referer)
{
    LOG_INFO << "[chaynsAPI] 正在上传图片到图片服务，personIdPresent=" << !personId.empty();
    
    // 如果已经有 URL，直接返回
    if (!image.uploadedUrl.empty()) {
        LOG_INFO << "[chaynsAPI] 图片已有已上传 URL，urlPresent=true";
        return image.uploadedUrl;
    }
    
    // 需要上传 图片
    if (image.base64Data.empty()) {
        LOG_ERROR << "[chaynsAPI] 没有图片数据可上传";
        return "";
    }
    
    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    
    // 构建上传请求

    std::string uploadPath = "/image-service/v3/Images/" + personId;
    
    // 首先解码 数据
    std::string decodedData = drogon::utils::base64Decode(image.base64Data);
    
    // 确定文件扩展名
    std::string extension = "png";
    if (image.mediaType.find("jpeg") != std::string::npos || image.mediaType.find("jpg") != std::string::npos) {
        extension = "jpg";
    } else if (image.mediaType.find("gif") != std::string::npos) {
        extension = "gif";
    } else if (image.mediaType.find("webp") != std::string::npos) {
        extension = "webp";
    }
    
    // 创建 HTTP 请求并构建 / 请求体
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(HttpMethod::Post);
    request->setPath(uploadPath);
    request->addHeader("Authorization", "Bearer " + authToken);
    chayns_browser::applyBrowserHeaders(
        request,
        chayns_browser::accountKeyFor(accountUserName, personId),
        origin,
        referer);
    

    std::string boundary = "----WebKitFormBoundary" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::string contentType = "multipart/form-data; boundary=" + boundary;
    request->setContentTypeString(contentType);
    
    std::string mimeType = "image/" + extension;
    if (extension == "jpg") mimeType = "image/jpeg";
    
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"image." + extension + "\"\r\n";
    body += "Content-Type: " + mimeType + "\r\n\r\n";
    body += decodedData;
    body += "\r\n--" + boundary + "--\r\n";
    
    request->setBody(body);
    
    auto [result, response] = client->sendRequest(request);
    
    if (result != ReqResult::Ok || !response) {
        LOG_ERROR << "[chaynsAPI] 上传图片失败： 网络错误";
        return "";
    }
    
    if (response->statusCode() != k200OK && response->statusCode() != k201Created) {
        LOG_ERROR << "[chaynsAPI] 上传图片失败： " << summarizeUpstreamResponse(response);
        return "";
    }
    
    // 解析响应获取图片URL
    auto jsonResp = response->getJsonObject();
    if (!jsonResp) {
        LOG_ERROR << "[chaynsAPI] 解析上传响应JSON失败";
        return "";
    }
    

    if (jsonResp->isMember("baseDomain") && jsonResp->isMember("image") && (*jsonResp)["image"].isMember("path")) {
        std::string baseDomain = (*jsonResp)["baseDomain"].asString();
        std::string imagePath = (*jsonResp)["image"]["path"].asString();
        std::string imageUrl = baseDomain + imagePath;
        LOG_INFO << "[chaynsAPI] 图片上传成功：urlPresent=true";
        return imageUrl;
    }
    
    LOG_ERROR << "[chaynsAPI] 上传响应格式异常";
    return "";
}

provider::ProviderResult chaynsapi::generate(session_st& session)
{
    postChatMessage(session);

    provider::ProviderResult result;
    result.text = session.response.message.get("message", "").asString();
    result.statusCode = session.response.message.get("statusCode", 500).asInt();

    if (result.statusCode == 200) {
        result.error = provider::ProviderError::none();
    } else {
        const std::string errorMessage = session.response.message.get(
            "error", "Provider returned error").asString();
        const std::string providerCode = session.response.message.get(
            "errorCode", "").asString();
        provider::ProviderErrorCode errorCode = provider::ProviderErrorCode::InternalError;
        if (result.statusCode == 400 || result.statusCode == 404) {
            errorCode = provider::ProviderErrorCode::InvalidRequest;
        } else if (result.statusCode == 401 || result.statusCode == 403) {
            errorCode = provider::ProviderErrorCode::AuthError;
        } else if (result.statusCode == 429) {
            errorCode = provider::ProviderErrorCode::RateLimited;
        } else if (result.statusCode == 503) {
            errorCode = provider::ProviderErrorCode::ServiceUnavailable;
        } else if (result.statusCode == 504) {
            errorCode = provider::ProviderErrorCode::Timeout;
        }
        result.error = provider::ProviderError{
            errorCode, errorMessage, providerCode, result.statusCode};
    }

    return result;
}

void chaynsapi::postChatMessage(session_st& session)
{
    LOG_INFO << "[chaynsAPI] 发送聊天消息";
    string modelname = session.request.model;

    // Resolve and validate the model before consuming an account or uploading
    // attachments. Unknown models get one forced catalog refresh so newly
    // published upstream models become usable without a process restart.
    chayns::ModelDescriptor selectedModel;
    if (!findModel(modelname, selectedModel)) {
        loadModels(true);
    }
    if (!findModel(modelname, selectedModel)) {
        LOG_ERROR << "[chaynsAPI] 未找到请求模型: " << modelname;
        setChaynsSessionError(session, 400, "model_not_found", "Unknown model: " + modelname);
        return;
    }

    if (!session.request.images.empty()) {
        if (!chayns::supportsImageInput(selectedModel)) {
            LOG_WARN << "[chaynsAPI] 模型不支持图片输入: " << modelname;
            setChaynsSessionError(
                session, 400, "unsupported_capability", "Model does not support image input");
            return;
        }
        for (const auto& image : session.request.images) {
            if (!image.mediaType.empty() && !chayns::supportsMimeType(selectedModel, image.mediaType)) {
                LOG_WARN << "[chaynsAPI] 模型不支持附件类型: model=" << modelname
                         << ", mime=" << image.mediaType;
                setChaynsSessionError(
                    session, 400, "unsupported_mime_type",
                    "Model does not support MIME type: " + image.mediaType);
                return;
            }
        }
    }
    
    // ========== 上游重试外层循环 ==========
    // POST 一旦返回消息锚点，就只重试 GET 轮询，绝不重新发送尚未得到最终
    // 回复的用户消息。只有 POST 被明确拒绝，或已收到可判定的上游错误最终
    // 消息时，外层循环才允许创建新线程/切换账号重试。
    int totalAttempts = 0;
    int consecutiveFails = 0;  // 跨线程的连续失败计数，用于判断是否需要换账号
    bool upstreamSuccess = false;
    bool fatalAmbiguousSend = false;
    bool fatalCorrelationConflict = false;
    bool fatalResponseTimeout = false;
    // Queueing behind another request on the same account must not consume the
    // upstream polling budget.  The deadline starts only after the first
    // account lease has been acquired.
    auto requestDeadline = std::chrono::steady_clock::time_point::max();
    bool requestDeadlineStarted = false;
    
    // 最终结果保存
    string final_response_message;
    int final_response_statusCode = 204;
    string final_threadId;
    string final_userAuthorId;
    string final_agentAuthorId;
    string final_accountUserName;
    string final_accountType;
    int final_threadTypeId = 8;
    std::int64_t final_workspaceUacId = 0;
    string final_origin;
    string final_referer;
    string final_requestMessageId;
    string final_requestCreationTime;
    string final_assistantMessageId;
    Json::Value final_reasoningMessages(Json::arrayValue);
    
    // 上传的图片URL（在首次尝试时上传，后续重试复用）
    std::vector<std::string> uploadedImageUrls;
    bool imagesUploaded = false;

    const AccountRequirement accountRequirement = selectedModel.requiresPro
        ? AccountRequirement::ProOnly
        : AccountRequirement::FreeOnly;
    std::set<std::string> attemptedAccounts;
    shared_ptr<Accountinfo_st> selectedAccount;

    ThreadContext continuationContext;
    bool hasContinuationContext = false;
    if (session.state.isContinuation && !session.provider.prevProviderKey.empty()) {
        std::lock_guard<std::mutex> lock(m_threadMapMutex);
        auto it = m_threadMap.find(session.provider.prevProviderKey);
        if (it != m_threadMap.end()) {
            continuationContext = it->second;
            hasContinuationContext = true;
        }
    }
    
    std::unique_lock<std::mutex> accountExecutionLock;
    while (totalAttempts < MAX_UPSTREAM_RETRIES && !upstreamSuccess &&
           (!requestDeadlineStarted || std::chrono::steady_clock::now() < requestDeadline)) {
        totalAttempts++;
        final_threadId.clear();
        final_userAuthorId.clear();
        final_agentAuthorId.clear();
        final_accountUserName.clear();
        final_accountType.clear();
        final_threadTypeId = 8;
        final_workspaceUacId = 0;
        final_origin.clear();
        final_referer.clear();
        final_requestMessageId.clear();
        final_requestCreationTime.clear();
        final_assistantMessageId.clear();
        final_reasoningMessages = Json::Value(Json::arrayValue);
        bool needSwitchAccount = (consecutiveFails >= CONSECUTIVE_FAILS_BEFORE_SWITCH);
        
        if (totalAttempts > 1) {
            LOG_INFO << "[chaynsAPI] 上游重试第" << totalAttempts << " 次 (连续失败: " << consecutiveFails 
                     << ", 需要换账号: " << (needSwitchAccount ? "是" : "否") << ")";
        }
        
        // ---- 1. 获取满足模型权限要求的账号 ----
        shared_ptr<Accountinfo_st> previousAccount;
        if (needSwitchAccount && selectedAccount) {
            previousAccount = selectedAccount;
            attemptedAccounts.insert(selectedAccount->userName);
            selectedAccount.reset();
        }

        if (!isUsableChaynsAccount(selectedAccount, selectedModel.requiresPro)) {
            selectedAccount.reset();

            // A continuation may only reuse its original account when that
            // account is still valid and satisfies the current model.
            if (totalAttempts == 1 && !needSwitchAccount && hasContinuationContext &&
                !continuationContext.accountUserName.empty()) {
                AccountManager::getInstance().getAccountByUserName(
                    "chaynsapi", continuationContext.accountUserName, selectedAccount);
                if (!isUsableChaynsAccount(selectedAccount, selectedModel.requiresPro)) {
                    LOG_WARN << "[chaynsAPI] 续聊账户不可用或权限不足: "
                             << continuationContext.accountUserName;
                    selectedAccount.reset();
                }
            }

            if (!selectedAccount && !AccountManager::getInstance().getEligibleAccount(
                    "chaynsapi", selectedAccount, accountRequirement, attemptedAccounts)) {
                // Switching is a retry preference, not an additional account
                // requirement. If there is only one eligible account, finish
                // the retry budget with it instead of misreporting a 503.
                if (isUsableChaynsAccount(previousAccount, selectedModel.requiresPro)) {
                    selectedAccount = previousAccount;
                    LOG_WARN << "[chaynsAPI] 没有可切换的其它账户，继续使用当前账户: "
                             << selectedAccount->userName;
                } else {
                    LOG_ERROR << "[chaynsAPI] 没有满足模型要求的有效账户: model=" << modelname
                              << ", requiresPro=" << selectedModel.requiresPro;
                    setChaynsSessionError(
                        session, 503, "account_unavailable",
                        selectedModel.requiresPro
                            ? "No valid Pro account with workspaceUacId available for the requested model"
                            : "No valid Free account available for the requested model");
                    return;
                }
            }
        }

        shared_ptr<Accountinfo_st> accountinfo = selectedAccount;
        const ChaynsRequestRoute requestRoute = requestRouteForAccount(*accountinfo);
        LOG_INFO << "[chaynsAPI] 已选择请求路由: accountType="
                 << accountinfo->accountType
                 << ", threadTypeId=" << requestRoute.threadTypeId
                 << ", workspaceUacIdConfigured="
                 << (requestRoute.workspaceUacId > 0);
        if (requestRoute.isPro && requestRoute.workspaceUacId <= 0) {
            LOG_ERROR << "[chaynsAPI] Pro 账号请求缺少 workspaceUacId 配置";
            setChaynsSessionError(
                session,
                503,
                "chayns_pro_workspace_not_configured",
                "Chayns Pro workspaceUacId is not configured");
            return;
        }

        if (accountExecutionLock.owns_lock()) {
            accountExecutionLock.unlock();
        }
        const auto accountGate = accountExecutionGate(accountinfo->userName);
        const auto accountWaitStartedAt = std::chrono::steady_clock::now();
        accountExecutionLock = std::unique_lock<std::mutex>(*accountGate);
        const auto accountWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - accountWaitStartedAt).count();
        LOG_INFO << "[chaynsAPI] 已获取账号单飞租约: waitMs=" << accountWaitMs;
        if (!requestDeadlineStarted) {
            requestDeadline = std::chrono::steady_clock::now() +
                              chayns::kRequestPollingDeadline;
            requestDeadlineStarted = true;
        }
        

        if (accountinfo->personId.empty()) {
            LOG_INFO << "[chaynsAPI] personId为空，正在尝试获取";
            auto authClient = HttpClient::newHttpClient("https://auth.chayns.net");
            auto request = HttpRequest::newHttpRequest();
            request->setMethod(HttpMethod::Get);
            request->setPath("/v2/userSettings");
            request->addHeader("Authorization", "Bearer " + accountinfo->authToken);
            applyChaynsRouteHeaders(request, *accountinfo, requestRoute);
            auto [result, response] = authClient->sendRequest(request);
            if (result == ReqResult::Ok && response && response->statusCode() == k200OK) {
                auto jsonResp = response->getJsonObject();
                if (jsonResp) {
                    if (jsonResp->isMember("personId")) {
                        accountinfo->personId = (*jsonResp)["personId"].asString();
                        LOG_INFO << "[chaynsAPI] 成功获取 personId：personIdPresent=true";
                    } else {
                        LOG_ERROR << "[chaynsAPI] 用户设置响应JSON中未找到 personId，"
                                  << summarizeUpstreamResponse(response);
                    }
                } else {
                    LOG_ERROR << "[chaynsAPI] 解析用户设置响应为JSON对象失败，"
                              << summarizeUpstreamResponse(response);
                }
            } else {
                LOG_ERROR << "[chaynsAPI] 获取用户设置失败，"
                          << summarizeUpstreamResponse(response);
            }
        }
        
        if (accountinfo->personId.empty()) {
            LOG_ERROR << "[chaynsAPI] 尝试获取后personId仍为空，中止当前尝试";
            consecutiveFails++;
            if (totalAttempts >= MAX_UPSTREAM_RETRIES) {
                session.response.message["error"] = "Failed to obtain a valid personId";
                session.response.message["statusCode"] = 500;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY * 5));
            continue;
        }
        
        // ---- 3. 处理图片上传 (仅首次) ----
        if (!imagesUploaded && !session.request.images.empty()) {
            LOG_INFO << "[chaynsAPI] 正在处理" << session.request.images.size() << " 张图片上传";
            for (auto& img : session.request.images) {
                std::string imageUrl = uploadImageToService(
                    img,
                    accountinfo->personId,
                    accountinfo->authToken,
                    accountinfo->userName,
                    requestRoute.origin,
                    requestRoute.referer);
                if (!imageUrl.empty()) {
                    uploadedImageUrls.push_back(imageUrl);
                    img.uploadedUrl = imageUrl;
                }
            }
            LOG_INFO << "[chaynsAPI] 成功上传" << uploadedImageUrls.size() << " 张图片";
            imagesUploaded = true;
        }
        
        // ---- 4. 发送请求（首次发送） ----
        auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
        string threadId;
        string userAuthorId;
        string agentAuthorId;
        string lastMessageTime;
        string requestMessageId;
        
        // 只在首次尝试且未要求换账号时，尝试使用已有线程。
        // 路由上下文必须完整一致，旧版上下文也会安全地创建新线程。
        bool isFollowUp = false;
        const bool continuationRouteMatches =
            continuationContext.accountType == accountinfo->accountType &&
            continuationContext.threadTypeId == requestRoute.threadTypeId &&
            continuationContext.origin == requestRoute.origin &&
            continuationContext.referer == requestRoute.referer &&
            (!requestRoute.isPro ||
             continuationContext.workspaceUacId == requestRoute.workspaceUacId);
        if (totalAttempts == 1 && !needSwitchAccount && hasContinuationContext &&
            accountinfo->userName == continuationContext.accountUserName &&
            (continuationContext.modelId.empty() || continuationContext.modelId == modelname) &&
            continuationRouteMatches) {
            threadId = continuationContext.threadId;
            userAuthorId = continuationContext.userAuthorId;
            agentAuthorId = continuationContext.agentAuthorId;
            isFollowUp = true;
            LOG_INFO << "[chaynsAPI] 找到现有线程: threadIdPresent=" << !threadId.empty()
                     << ", previousProviderPresent=" << !session.provider.prevProviderKey.empty();
        } else if (hasContinuationContext && !continuationRouteMatches) {
            LOG_INFO << "[chaynsAPI] 续聊请求路由发生变化，将创建新线程: oldAccountType="
                     << continuationContext.accountType
                     << ", newAccountType=" << accountinfo->accountType
                     << ", oldThreadTypeId=" << continuationContext.threadTypeId
                     << ", newThreadTypeId=" << requestRoute.threadTypeId;
        } else if (hasContinuationContext && continuationContext.modelId != modelname) {
            LOG_INFO << "[chaynsAPI] 续聊请求模型发生变化，将创建新线程: old="
                     << continuationContext.modelId << ", new=" << modelname;
        }
        
        Json::Value sendResponseJson;
        bool sendFailed = false;
        
        if (isFollowUp) {
            // =================================================
            // 分支 A： 后续对话 (发送消息到现有 线程)
            // =================================================
            Json::Value messageBody;
            string messageText = session.request.message;
            
            messageBody["text"] = messageText;
            LOG_INFO << "[chaynsAPI] 发送后续消息: textLength=" << messageText.size();
            messageBody["cursorPosition"] = messageText.size();
            
            if (!uploadedImageUrls.empty()) {
                Json::Value imagesArray(Json::arrayValue);
                for (const auto& url : uploadedImageUrls) {
                    Json::Value imgObj;
                    imgObj["url"] = url;
                    imagesArray.append(imgObj);
                }
                messageBody["images"] = imagesArray;
                LOG_INFO << "[chaynsAPI] 已添加" << uploadedImageUrls.size() << " 张图片到后续消息";
            }
            
            auto reqSend = HttpRequest::newHttpJsonRequest(messageBody);
            reqSend->setMethod(HttpMethod::Post);
            string path = "/intercom-backend/v2/thread/" + threadId + "/message";
            reqSend->setPath(path);
            reqSend->addHeader("Authorization", "Bearer " + accountinfo->authToken);
            applyChaynsRouteHeaders(reqSend, *accountinfo, requestRoute);
            
            LOG_INFO << "[chaynsAPI] 正在发送后续消息到线程: threadIdPresent=" << !threadId.empty();
            
            auto sendResult = client->sendRequest(reqSend);
            if (sendResult.first != ReqResult::Ok || !sendResult.second) {
                LOG_ERROR << "[chaynsAPI] 发送后续消息失败(网络错误)";
                sendFailed = true;
                fatalAmbiguousSend = true;
            } else {
                auto responseSend = sendResult.second;
                if (responseSend->statusCode() == k200OK || responseSend->statusCode() == k201Created) {
                    auto sendJson = responseSend->getJsonObject();
                    if (!sendJson) {
                        LOG_ERROR << "[chaynsAPI] 后续消息发送成功但响应JSON为空";
                        sendFailed = true;
                        fatalAmbiguousSend = true;
                    } else {
                        sendResponseJson = *sendJson;
                    }
                    if (sendResponseJson.isMember("creationTime")) {
                        lastMessageTime = sendResponseJson["creationTime"].asString();
                    }
                    if (sendResponseJson.isMember("id") && sendResponseJson["id"].isString()) {
                        requestMessageId = sendResponseJson["id"].asString();
                    }
                    if (sendResponseJson.isMember("author") && sendResponseJson["author"].isMember("id")) {
                        userAuthorId = sendResponseJson["author"]["id"].asString();
                    }
                } else {
                    LOG_ERROR << "[chaynsAPI] 后续消息发送失败，"
                              << summarizeUpstreamResponse(responseSend);
                    sendFailed = true;
                    fatalAmbiguousSend = postFailureMayHaveBeenAccepted(
                        responseSend->statusCode());
                }
            }
        } else {
            // =================================================
            // 分支 B： 新对话 (创建新 线程)
            // =================================================
            LOG_INFO << "[chaynsAPI] 正在创建新线程： 注入系统提示词 (" << session.request.systemPrompt.length() << " 字符)";
            string full_message;
            const std::string historyHeader = "\n接下来是 OpenAI 接口格式的历史消息：\n";
            const std::string currentHeader = "\n用户现在的问题是:\n";
            const size_t fixedBytes = session.request.systemPrompt.size() +
                historyHeader.size() + currentHeader.size() + session.request.message.size();
            const size_t historyBudget = continuity::remainingHistoryBudget(
                continuity::historyReplayMaxRequestBytes(),
                fixedBytes
            );
            const std::string currentHistoryMessage = session.request.rawMessage.empty()
                ? session.request.message
                : session.request.rawMessage;
            const auto historySelection = continuity::selectRecentHistory(
                session.provider.messageContext,
                historyBudget,
                continuity::historyReplayMaxMessageBytes(),
                false,
                currentHistoryMessage
            );
            const bool historyIncluded = !historySelection.messages.empty();

            if (historyIncluded) {
                Json::StreamWriterBuilder historyWriter;
                historyWriter["indentation"] = "";
                full_message = session.request.systemPrompt + historyHeader +
                    Json::writeString(historyWriter, historySelection.messages) +
                    currentHeader + session.request.message;
            } else {
                full_message = session.request.systemPrompt + "\n" + session.request.message;
            }

            LOG_INFO << "[chaynsAPI] 新线程历史预算: original="
                     << historySelection.originalMessages
                     << ", selected=" << historySelection.selectedMessages
                     << ", selectedTurns=" << historySelection.selectedTurns
                     << ", selectedBytes=" << historySelection.selectedBytes
                     << ", replacedOversize=" << historySelection.skippedOversizeMessages
                     << ", normalizedTools=" << historySelection.normalizedToolMessages
                     << ", skippedForBudget=" << historySelection.skippedForBudget
                     << ", omissionNotice=" << historySelection.omissionNoticeAdded
                     << ", skippedDuplicateCurrent=" << historySelection.skippedDuplicateCurrentMessage
                     << ", requestTextBytes=" << full_message.size();
            
            Json::Value sendMessageRequest;
            Json::Value member1;
            member1["isAdmin"] = true;
            member1["personId"] = accountinfo->personId;
            sendMessageRequest["members"].append(member1);
            
            Json::Value member2;
            member2["personId"] = selectedModel.personId;
            sendMessageRequest["members"].append(member2);
            sendMessageRequest["nerMode"] = "None";
            sendMessageRequest["priority"] = 0;
            sendMessageRequest["typeId"] = requestRoute.threadTypeId;
            if (requestRoute.isPro) {
                sendMessageRequest["workspaceUacId"] =
                    Json::Int64(requestRoute.workspaceUacId);
            }
            
            Json::Value message;
            message["text"] = full_message;
            LOG_INFO << "[chaynsAPI] 发送新线程消息: textLength=" << full_message.size()
                      << ", historyPresent=" << historyIncluded;
            
            if (!uploadedImageUrls.empty()) {
                Json::Value imagesArray(Json::arrayValue);
                for (const auto& url : uploadedImageUrls) {
                    Json::Value imgObj;
                    imgObj["url"] = url;
                    imagesArray.append(imgObj);
                }
                message["images"] = imagesArray;
                LOG_INFO << "[chaynsAPI] 已添加" << uploadedImageUrls.size() << " 张图片到新线程消息";
            }
            
            sendMessageRequest["messages"].append(message);
            
            auto reqSend = HttpRequest::newHttpJsonRequest(sendMessageRequest);
            reqSend->setMethod(HttpMethod::Post);
            reqSend->setPath("/intercom-backend/v2/thread?forceCreate=true");
            reqSend->addHeader("Authorization", "Bearer " + accountinfo->authToken);
            applyChaynsRouteHeaders(reqSend, *accountinfo, requestRoute);
            
            LOG_INFO << "[chaynsAPI] 正在创建新线程: threadTypeId="
                     << requestRoute.threadTypeId
                     << ", workspaceUacIdIncluded=" << requestRoute.isPro;
            
            auto sendResult = client->sendRequest(reqSend);
            if (sendResult.first == ReqResult::Ok && sendResult.second &&
                static_cast<int>(sendResult.second->statusCode()) == 413 && historyIncluded) {
                const std::string fallbackMessage =
                    session.request.systemPrompt + "\n" + session.request.message;
                LOG_WARN << "[chaynsAPI] 新线程请求因历史负载返回 413，去除历史后重试: originalBytes="
                         << full_message.size() << ", retryBytes=" << fallbackMessage.size();

                message["text"] = fallbackMessage;
                sendMessageRequest["messages"][0] = message;
                auto retryReq = HttpRequest::newHttpJsonRequest(sendMessageRequest);
                retryReq->setMethod(HttpMethod::Post);
                retryReq->setPath("/intercom-backend/v2/thread?forceCreate=true");
                retryReq->addHeader("Authorization", "Bearer " + accountinfo->authToken);
                applyChaynsRouteHeaders(retryReq, *accountinfo, requestRoute);
                sendResult = client->sendRequest(retryReq);
            }

            if (sendResult.first != ReqResult::Ok || !sendResult.second) {
                LOG_ERROR << "[chaynsAPI] 创建线程失败(网络错误)";
                sendFailed = true;
                fatalAmbiguousSend = true;
            } else {
                auto responseSend = sendResult.second;
                if (responseSend->statusCode() == k200OK || responseSend->statusCode() == k201Created) {
                    auto sendJson = responseSend->getJsonObject();
                    if (!sendJson) {
                        LOG_ERROR << "[chaynsAPI] 创建线程成功但响应JSON为空";
                        sendFailed = true;
                        fatalAmbiguousSend = true;
                    } else {
                        sendResponseJson = *sendJson;
                    }
                    if (sendResponseJson.isMember("id")) {
                        threadId = sendResponseJson["id"].asString();
                        
                        if (sendResponseJson.isMember("members") && sendResponseJson["members"].isArray()) {
                            for (const auto& member : sendResponseJson["members"]) {
                                if (member.isMember("personId") && member["personId"].asString() == accountinfo->personId) {
                                    if (member.isMember("id") && member["id"].isString()) {
                                        userAuthorId = member["id"].asString();
                                    }
                                }
                                if (member.isMember("personId") && member["personId"].asString() == selectedModel.personId &&
                                    member.isMember("id") && member["id"].isString()) {
                                    agentAuthorId = member["id"].asString();
                                }
                            }
                        }
                        
                        if (sendResponseJson.isMember("messages") && sendResponseJson["messages"].isArray() && sendResponseJson["messages"].size() > 0) {
                            const auto& sentMessage = sendResponseJson["messages"][0];
                            lastMessageTime = sentMessage.get("creationTime", "").asString();
                            requestMessageId = sentMessage.get("id", "").asString();
                            if (sentMessage.isMember("author") && sentMessage["author"].isObject()) {
                                userAuthorId = sentMessage["author"].get("id", userAuthorId).asString();
                            }
                        }
                    }
                } else {
                    LOG_ERROR << "[chaynsAPI] 创建线程失败，状态码：" << responseSend->statusCode();
                    sendFailed = true;
                    fatalAmbiguousSend = postFailureMayHaveBeenAccepted(
                        responseSend->statusCode());
                }
            }
        }
        
        // 如果首次发送就失败了（网络错误等），直接进入外层重试
        if (sendFailed) {
            consecutiveFails++;
            LOG_WARN << "[chaynsAPI] 发送请求失败，连续失败次数：" << consecutiveFails;
            if (fatalAmbiguousSend) {
                LOG_ERROR << "[chaynsAPI] POST 结果不确定，禁止自动重发以避免同账号产生重叠生成";
                break;
            }
            if (consecutiveFails >= CONSECUTIVE_FAILS_BEFORE_SWITCH) {
                LOG_WARN << "[chaynsAPI] 连续失败" << consecutiveFails << " 次, 下次将切换账号";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY * 5));
            continue;
        }
        
        if (threadId.empty() || lastMessageTime.empty() || requestMessageId.empty() || userAuthorId.empty()) {
            LOG_ERROR << "[chaynsAPI] 关键信息缺失：线程、用户消息锚点或作者信息不完整";
            fatalAmbiguousSend = true;
            break;
        }
        
        // ========== 5. 根据 POST 返回的消息锚点轮询 ==========
        chayns::MessageAnchor messageAnchor;
        messageAnchor.messageId = requestMessageId;
        messageAnchor.threadId = threadId;
        messageAnchor.userAuthorId = userAuthorId;
        messageAnchor.agentAuthorId = agentAuthorId;
        messageAnchor.creationTime = lastMessageTime;
        std::unordered_set<std::string> consumedMessageIds;
        Json::Value reasoningMessages(Json::arrayValue);
        string response_message;
        int response_statusCode = 204;
        int pollCount = 0;
        bool pollFound = false;

        const string pollPath = "/intercom-backend/v2/thread/" + threadId + "/message";
        const auto pollingStartedAt = std::chrono::steady_clock::now();
        LOG_INFO << "[chaynsAPI] 开始轮询锚定消息，请求总截止时间: "
                 << std::chrono::duration_cast<std::chrono::seconds>(
                        chayns::kRequestPollingDeadline).count()
                 << " 秒";
        LOG_INFO << "[chaynsAPI] 轮询路径: " << pollPath+"&take=1000&viewMode=user&afterDate="+lastMessageTime;

        while (std::chrono::steady_clock::now() < requestDeadline) {
            pollCount++;
            auto reqGet = HttpRequest::newHttpRequest();
            reqGet->setMethod(HttpMethod::Get);
            reqGet->setPath(pollPath);
            reqGet->setParameter("take", "1000");
            reqGet->setParameter("viewMode", "user");
            reqGet->setParameter("afterDate", lastMessageTime);
            reqGet->addHeader("Authorization", "Bearer " + accountinfo->authToken);
            applyChaynsRouteHeaders(reqGet, *accountinfo, requestRoute);

            auto getResult = client->sendRequest(reqGet);
            if (getResult.first == ReqResult::Ok && getResult.second) {
                auto responseGet = getResult.second;
                if (responseGet->statusCode() == k200OK) {
                    auto jsonResp = responseGet->getJsonObject();
                    if (jsonResp && jsonResp->isArray() && !jsonResp->empty()) {
                        const auto correlated = chayns::correlateMessageBatch(
                            *jsonResp, messageAnchor, consumedMessageIds);
                        if (messageAnchor.agentAuthorId.empty() &&
                            !correlated.inferredAgentAuthorId.empty()) {
                            messageAnchor.agentAuthorId = correlated.inferredAgentAuthorId;
                        }
                        for (const auto& reasoning : correlated.reasoningMessages) {
                            reasoningMessages.append(reasoning);
                        }
                        if (correlated.status == chayns::CorrelationStatus::Superseded) {
                            LOG_ERROR << "[chaynsAPI] 当前消息最终回复前出现另一条用户消息，拒绝猜测回复归属";
                            response_statusCode = 409;
                            fatalCorrelationConflict = true;
                            pollFound = true;
                            break;
                        }
                        if (correlated.status == chayns::CorrelationStatus::FinalFound) {
                            const auto& msg = correlated.finalMessage;
                            response_message = msg.get("text", "").asString();
                            final_assistantMessageId = msg.get("id", "").asString();
                            response_statusCode = 200;
                            pollFound = true;
                            LOG_INFO << "[chaynsAPI] 轮询结束，总计轮询" << pollCount
                                     << " 次, 成功获取锚定响应"
                                     << ", reasoningCount=" << reasoningMessages.size();
                            LOG_INFO << "[chaynsAPI] 回复已接收: textLength="
                                     << response_message.size();
                            LOG_DEBUG << "[chaynsAPI] 回复内容: " << response_message;
                            break;
                        }
                    }
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= requestDeadline) {
                break;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - pollingStartedAt);
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                requestDeadline - now);
            std::this_thread::sleep_for(
                std::min(chayns::pollingDelayForElapsed(elapsed), remaining));
        }

        if (!pollFound) {
            LOG_INFO << "[chaynsAPI] 轮询结束，总计轮询" << pollCount << " 次, 未获取到响应";
            fatalResponseTimeout = true;
        }

        // Preserve the exact upstream anchor and any reasoning observed even
        // when correlation ends in a conflict or timeout.
        final_threadId = threadId;
        final_userAuthorId = userAuthorId;
        final_agentAuthorId = messageAnchor.agentAuthorId;
        final_accountUserName = accountinfo->userName;
        final_accountType = accountinfo->accountType;
        final_threadTypeId = requestRoute.threadTypeId;
        final_workspaceUacId = requestRoute.workspaceUacId;
        final_origin = requestRoute.origin;
        final_referer = requestRoute.referer;
        final_requestMessageId = requestMessageId;
        final_requestCreationTime = lastMessageTime;
        final_reasoningMessages = reasoningMessages;

        bool isUpstreamError = response_statusCode != 200;
        if (response_statusCode == 409) {
            LOG_WARN << "[chaynsAPI] 上游线程消息顺序冲突，当前响应不具备确定关联";
        } else if (response_statusCode != 200) {
            LOG_WARN << "[chaynsAPI] 上游错误：轮询超时未获取响应 (线程Id："
                     << threadId << ")";
        } else {
            for (const auto& errorText : m_upstreamErrorTexts) {
                if (response_message == errorText) {
                    isUpstreamError = true;
                    LOG_WARN << "[chaynsAPI] 上游错误：收到错误文本 '" << errorText
                             << "' (线程Id：" << threadId << ")";
                    break;
                }
            }
        }

        if (!isUpstreamError) {
            upstreamSuccess = true;
            final_response_message = response_message;
            final_response_statusCode = response_statusCode;
            LOG_INFO << "[chaynsAPI] 上游请求成功 (外层第" << totalAttempts << " 次)";
        }
        
        // 已取得当前锚点对应的最终回复，结束外层循环。
        if (upstreamSuccess) {
            break;
        }
        if (fatalAmbiguousSend || fatalCorrelationConflict || fatalResponseTimeout) {
            break;
        }
        
        // POST 被明确拒绝或已经收到错误最终消息时，才允许外层重试。
        // 轮询超时会耗尽总截止时间，不会再次 POST。
        consecutiveFails++;
        LOG_WARN << "[chaynsAPI] 当前上游尝试失败, 连续失败次数: " << consecutiveFails
                 << ", 总尝试次数: " << totalAttempts << "/" << MAX_UPSTREAM_RETRIES;
        
        if (consecutiveFails >= CONSECUTIVE_FAILS_BEFORE_SWITCH) {
            LOG_WARN << "[chaynsAPI] 连续失败" << consecutiveFails << " 次, 下次将切换账号并创建新会话";
        } else {
            LOG_INFO << "[chaynsAPI] 下次将使用同一账号创建新线程重试";
        }
        
        // 添加延迟避免过于频繁的重试
        if (std::chrono::steady_clock::now() < requestDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY * 10));
        }
        
    } // 外层重试循环结束（totalAttempts < MAX_UPSTREAM_RETRIES）

    if (!upstreamSuccess && std::chrono::steady_clock::now() >= requestDeadline) {
        LOG_WARN << "[chaynsAPI] 请求已达到 "
                 << std::chrono::duration_cast<std::chrono::seconds>(
                        chayns::kRequestPollingDeadline).count()
                 << " 秒总截止时间";
    }
    
    // ========== 重试循环结束，处理最终结果 ==========
    
    if (upstreamSuccess) {
        // 更新上下文映射表
        {
            std::lock_guard<std::mutex> lock(m_threadMapMutex);
            ThreadContext ctx;
            ctx.threadId = final_threadId;
            ctx.userAuthorId = final_userAuthorId;
            ctx.agentAuthorId = final_agentAuthorId;
            ctx.accountUserName = final_accountUserName;
            ctx.modelId = modelname;
            ctx.accountType = final_accountType;
            ctx.threadTypeId = final_threadTypeId;
            ctx.workspaceUacId = final_workspaceUacId;
            ctx.origin = final_origin;
            ctx.referer = final_referer;
            ctx.lastRequestMessageId = final_requestMessageId;
            ctx.lastRequestCreationTime = final_requestCreationTime;
            ctx.lastAssistantMessageId = final_assistantMessageId;
            ctx.lastReasoningMessages = final_reasoningMessages;
            m_threadMap[session.state.conversationId] = ctx;
        }
        
        session.response.message["message"] = final_response_message;
        session.response.message["statusCode"] = final_response_statusCode;
        auto& chaynsMeta = session.response.message["_meta"]["chayns"];
        chaynsMeta["request_message_id"] = final_requestMessageId;
        chaynsMeta["request_creation_time"] = final_requestCreationTime;
        chaynsMeta["assistant_message_id"] = final_assistantMessageId;
        chaynsMeta["reasoning_messages"] = final_reasoningMessages;
        chaynsMeta["account_type"] = final_accountType;
        chaynsMeta["thread_type_id"] = final_threadTypeId;
        if (final_accountType == "pro") {
            chaynsMeta["workspace_uac_id"] = Json::Int64(final_workspaceUacId);
        }
    } else {
        LOG_ERROR << "[chaynsAPI] 所有上游重试均失败 (总尝试次数：" << totalAttempts 
                 << "/" << MAX_UPSTREAM_RETRIES << ")";
        if (fatalAmbiguousSend || fatalCorrelationConflict || fatalResponseTimeout) {
            // The old upstream thread may still receive a delayed response.
            // Remove its continuation mapping so the next request cannot reuse
            // a thread whose message ordering is no longer deterministic.
            std::lock_guard<std::mutex> lock(m_threadMapMutex);
            if (!session.provider.prevProviderKey.empty()) {
                m_threadMap.erase(session.provider.prevProviderKey);
            }
            m_threadMap.erase(session.state.conversationId);
        }
        if (!final_requestMessageId.empty()) {
            auto& chaynsMeta = session.response.message["_meta"]["chayns"];
            chaynsMeta["request_message_id"] = final_requestMessageId;
            chaynsMeta["request_creation_time"] = final_requestCreationTime;
            chaynsMeta["reasoning_messages"] = final_reasoningMessages;
            chaynsMeta["account_type"] = final_accountType;
            chaynsMeta["thread_type_id"] = final_threadTypeId;
            if (final_accountType == "pro") {
                chaynsMeta["workspace_uac_id"] = Json::Int64(final_workspaceUacId);
            }
        }
        if (fatalCorrelationConflict) {
            session.response.message["error"] =
                "Upstream thread contains an overlapping user message";
            session.response.message["errorCode"] = "upstream_message_conflict";
            session.response.message["statusCode"] = 409;
        } else if (fatalAmbiguousSend) {
            session.response.message["error"] =
                "Upstream message submission outcome is ambiguous";
            session.response.message["errorCode"] = "upstream_send_ambiguous";
            session.response.message["statusCode"] = 502;
        } else if (fatalResponseTimeout) {
            session.response.message["error"] =
                "Timed out waiting for the anchored upstream response";
            session.response.message["errorCode"] = "upstream_response_timeout";
            session.response.message["statusCode"] = 504;
        } else {
            session.response.message["error"] = "Upstream failed after all retries";
            session.response.message["statusCode"] = 500;
        }
    }
}
void chaynsapi::checkAlivableTokens()
{

}
bool chaynsapi::checkAlivableToken(string token)
{
    auto client = HttpClient::newHttpClient("https://auth.chayns.net");
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(HttpMethod::Get);
    request->setPath("/v2/userSettings");
    request->addHeader("Authorization", "Bearer " + token);
    chayns_browser::applyBrowserHeaders(request);
    auto [result, response] = client->sendRequest(request);
    if (result != ReqResult::Ok || !response) {
        LOG_ERROR << "[chaynsAPI] 验证Token失败：网络错误";
        return false;
    }
    LOG_INFO << "[chaynsAPI] 验证Token响应：" << response->getStatusCode();
    if(response->getStatusCode()!=200)
    {
        return false;
    }
    return true;
}
void chaynsapi::checkModels()
{

}
bool chaynsapi::findModel(const std::string& modelName, chayns::ModelDescriptor& model) const
{
    std::shared_lock<std::shared_mutex> lock(m_modelCatalogMutex);
    const auto it = m_modelCatalog.byName.find(modelName);
    if (it == m_modelCatalog.byName.end()) {
        return false;
    }
    model = it->second;
    return true;
}

bool chaynsapi::loadModels(bool forceRefresh)
{
    if (!forceRefresh) {
        std::shared_lock<std::shared_mutex> lock(m_modelCatalogMutex);
        if (!m_modelCatalog.byName.empty() &&
            m_modelsLoadedAt.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() - m_modelsLoadedAt < MODEL_CACHE_TTL) {
            return true;
        }
    }

    // Only one request performs the upstream refresh. Other callers recheck
    // the cache after acquiring this lock.
    std::lock_guard<std::mutex> refreshLock(m_modelRefreshMutex);
    if (!forceRefresh) {
        std::shared_lock<std::shared_mutex> lock(m_modelCatalogMutex);
        if (!m_modelCatalog.byName.empty() &&
            m_modelsLoadedAt.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() - m_modelsLoadedAt < MODEL_CACHE_TTL) {
            return true;
        }
    }

    const auto refreshStartedAt = std::chrono::steady_clock::now();
    if (m_lastModelRefreshAttempt.time_since_epoch().count() != 0 &&
        refreshStartedAt - m_lastModelRefreshAttempt < MODEL_REFRESH_MIN_INTERVAL) {
        std::shared_lock<std::shared_mutex> lock(m_modelCatalogMutex);
        LOG_INFO << "[chaynsAPI] 模型目录刷新请求过于频繁，使用当前缓存";
        return !m_modelCatalog.byName.empty();
    }
    m_lastModelRefreshAttempt = refreshStartedAt;

    auto client = HttpClient::newHttpClient("https://cube.tobit.cloud");
    auto request = HttpRequest::newHttpRequest();
    
    request->setMethod(HttpMethod::Get);
    request->setPath("/chayns-ai-chatbot/nativeModelChatbot");
    chayns_browser::applyBrowserHeaders(request);
    
    auto [result, response] = client->sendRequest(request);
    
    if (result != ReqResult::Ok || !response || response->statusCode() != k200OK) {
        LOG_ERROR << "[chaynsAPI] 从API获取模型列表失败, result=" << static_cast<int>(result)
                  << ", status=" << (response ? static_cast<int>(response->statusCode()) : 0);
        return false;
    }
    
    Json::Value api_models;
    Json::Reader reader;
    if (!reader.parse(string(response->getBody()), api_models)) {
        LOG_ERROR << "[chaynsAPI] 解析模型API响应失败";
        return false;
    }

    auto parsed = chayns::parseModelCatalog(api_models);
    if (!parsed.valid || parsed.catalog.byName.empty()) {
        LOG_ERROR << "[chaynsAPI] 模型列表结构无效或没有可用模型, skipped=" << parsed.skipped;
        return false;
    }

    std::size_t proModels = 0;
    for (const auto& entry : parsed.catalog.byName) {
        if (entry.second.requiresPro) {
            ++proModels;
        }
    }

    const std::size_t warningLogLimit = 20;
    for (std::size_t index = 0;
         index < parsed.warnings.size() && index < warningLogLimit;
         ++index) {
        LOG_WARN << "[chaynsAPI] 模型目录解析告警: " << parsed.warnings[index];
    }
    if (parsed.warnings.size() > warningLogLimit) {
        LOG_WARN << "[chaynsAPI] 其余模型目录解析告警已省略: "
                 << (parsed.warnings.size() - warningLogLimit) << " 条";
    }
    if (!parsed.imageCapabilityConflictModels.empty()) {
        std::ostringstream models;
        for (std::size_t index = 0;
             index < parsed.imageCapabilityConflictModels.size();
             ++index) {
            if (index > 0) {
                models << ", ";
            }
            models << parsed.imageCapabilityConflictModels[index];
        }
        LOG_WARN << "[chaynsAPI] 图片能力元数据冲突: count="
                 << parsed.imageCapabilityConflictModels.size()
                 << ", models=[" << models.str()
                 << "]; 已启用 canHandleImages 回退策略";
    }

    const std::size_t modelCount = parsed.catalog.byName.size();
    {
        std::unique_lock<std::shared_mutex> lock(m_modelCatalogMutex);
        m_modelCatalog = std::move(parsed.catalog);
        m_modelsLoadedAt = std::chrono::steady_clock::now();

        ModelInfoMap.clear();
        for (const auto& entry : m_modelCatalog.byName) {
            modelInfo info;
            info.modelName = entry.first;
            info.status = true;
            ModelInfoMap[entry.first] = std::move(info);
        }
    }

    LOG_INFO << "[chaynsAPI] Native模型Chatbot模型加载成功: total=" << modelCount
             << ", pro=" << proModels
             << ", non_pro=" << (modelCount - proModels)
             << ", skipped=" << parsed.skipped
             << ", duplicates=" << parsed.duplicates;
    return true;
}

std::string generateGuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) {
        ss << dis(gen);
    }
    ss << "-";
    for (int i = 0; i < 4; i++) {
        ss << dis(gen);
    }
    ss << "-4";  // UUID 版本位：固定为 V4
    for (int i = 0; i < 3; i++) {
        ss << dis(gen);
    }
    ss << "-";
    ss << dis2(gen);  // UUID 变体位
    for (int i = 0; i < 3; i++) {
        ss << dis(gen);
    }
    ss << "-";
    for (int i = 0; i < 12; i++) {
        ss << dis(gen);
    }
    return ss.str();
}
void chaynsapi::transferThreadContext(const std::string& oldId, const std::string& newId)
{
    LOG_INFO << "[chaynsAPI] 正在尝试转移线程上下文，从" << oldId << " 到 " << newId;
    std::lock_guard<std::mutex> lock(m_threadMapMutex);
    auto it = m_threadMap.find(oldId);
    if (it != m_threadMap.end()) {
        m_threadMap[newId] = it->second;
        m_threadMap.erase(it);
        LOG_INFO << "[chaynsAPI] 成功转移线程上下文，从" << oldId << " 到 " << newId;
    }
    else
    {
        LOG_WARN << "[chaynsAPI] 转移线程上下文失败： oldId" << oldId << "在线程Map中未找到";
    }
}
void chaynsapi::afterResponseProcess(session_st& session)
{

}
void chaynsapi::eraseChatinfoMap(string ConversationId)
{
    std::lock_guard<std::mutex> lock(m_threadMapMutex);
    const auto erased = m_threadMap.erase(ConversationId);
    LOG_INFO << "[chaynsAPI] 删除会话映射： convId 删除数量=" << erased;
}
Json::Value chaynsapi::getModels()
{
    loadModels(false);
    std::shared_lock<std::shared_mutex> lock(m_modelCatalogMutex);
    if (m_modelCatalog.openAiResponse.isObject() &&
        m_modelCatalog.openAiResponse.isMember("data")) {
        return m_modelCatalog.openAiResponse;
    }

    Json::Value emptyResponse(Json::objectValue);
    emptyResponse["object"] = "list";
    emptyResponse["data"] = Json::Value(Json::arrayValue);
    return emptyResponse;
}
void* chaynsapi::createApi()
{
    chaynsapi* api=new chaynsapi();
    api->init();
    return api;
}
