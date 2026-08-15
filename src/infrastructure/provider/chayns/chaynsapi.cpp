#include <drogon/drogon.h>
#include <infrastructure/provider/chayns/chaynsapi.h>
#include <infrastructure/persistence/chaynsThread/chaynsThreadDbManager.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <mutex>
#include <infrastructure/provider/limits/HistoryReplayBudget.h>
#include <infrastructure/provider/limits/OutboundBudget.h>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <infrastructure/provider/chayns/ChaynsBrowserImpersonation.h>
using namespace drogon;
using std::string;

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

platform::Result<provider::ProviderResponse> chaynsFailure(
    platform::ErrorCode code,
    std::string message,
    std::string providerCode = {},
    int upstreamHttpStatus = 0,
    std::string detail = {})
{
    return platform::Result<provider::ProviderResponse>::failure(
        platform::Error(code,
                        std::move(message),
                        std::move(detail),
                        std::move(providerCode),
                        upstreamHttpStatus));
}

std::optional<platform::Error> interruptionError(
    const provider::ProviderCallContext& context)
{
    if (context.isCancelled()) {
        return platform::Error::cancelled("Chayns provider request cancelled");
    }
    if (context.deadlineExceeded()) {
        return platform::Error::timeout("Chayns provider request deadline exceeded");
    }
    return std::nullopt;
}

Json::Value historyForChayns(const std::vector<provider::ProviderMessage>& messages)
{
    Json::Value history(Json::arrayValue);
    for (const auto& message : messages) {
        Json::Value item(Json::objectValue);
        switch (message.role) {
            case provider::ProviderMessageRole::System: item["role"] = "system"; break;
            case provider::ProviderMessageRole::User: item["role"] = "user"; break;
            case provider::ProviderMessageRole::Assistant: item["role"] = "assistant"; break;
            case provider::ProviderMessageRole::Tool: item["role"] = "tool"; break;
        }
        item["content"] = message.text;
        if (!message.toolCallId.empty()) {
            item["tool_call_id"] = message.toolCallId;
        }
        history.append(std::move(item));
    }
    return history;
}

std::string compactJson(const Json::Value& value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
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

chaynsapi::chaynsapi(IAccountSelector& accountSelector,
                     std::shared_ptr<chayns::IChaynsHttpTransport> transport,
                     std::shared_ptr<chayns::IChaynsClock> clock,
                     std::shared_ptr<chaynsThreadDbManager> threadLedger,
                     FailureObserver failureObserver)
    : ProviderBase(std::move(failureObserver)),
      m_accountSelector(accountSelector),
      m_transport(std::move(transport)),
      m_clock(std::move(clock)),
      m_threadLedger(std::move(threadLedger))
{
    if (!m_transport) {
        throw std::invalid_argument("chaynsapi requires a non-null HTTP transport");
    }
    if (!m_clock) {
        throw std::invalid_argument("chaynsapi requires a non-null clock");
    }
}

platform::Result<void> chaynsapi::initialize()
{
    chayns_browser::reloadConfigFromDrogon();

    if (!loadModels(true)) {
        return platform::Result<void>::failure(platform::Error::providerError(
            "Failed to load Chayns model catalog", "model_catalog_initialization"));
    }

    // 从配置 custom_config.upstream_error_texts 加载上游错误文本列表
    m_upstreamErrorTexts.clear();
    const auto& customConfig = drogon::app().getCustomConfig();
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
    return platform::Result<void>::success();
}

provider::ProviderCapabilities chaynsapi::capabilities() const noexcept
{
    return provider::ProviderCapabilities{/*nativeToolCalls=*/false,
                                          /*upstreamHistory=*/true,
                                          /*supportsImages=*/true};
}

std::shared_ptr<std::mutex> chaynsapi::accountExecutionGate(
    const std::string& accountUserName)
{
    std::lock_guard<std::mutex> lock(m_accountGatesMutex);
    auto& gate = m_accountGates[accountUserName];
    if (!gate) {
        gate = std::make_shared<std::mutex>();
    }
    return gate;
}

chayns::HttpResult chaynsapi::sendWithinContext(
    const provider::ProviderCallContext& context,
    const std::string& baseUrl,
    const drogon::HttpRequestPtr& request,
    double maximumTimeoutSeconds)
{
    const auto remaining = context.remaining();
    const double remainingSeconds = static_cast<double>(remaining.count()) / 1000.0;
    const double timeoutSeconds = std::min(maximumTimeoutSeconds, remainingSeconds);
    if (timeoutSeconds <= 0.0 || context.isCancelled()) {
        return {drogon::ReqResult::BadResponse, nullptr};
    }
    return m_transport->send(baseUrl, request, timeoutSeconds);
}


std::string chaynsapi::uploadImageToService(const ImageInfo& image,
                                            const std::string& personId,
                                            const std::string& authToken,
                                            const std::string& accountUserName,
                                            const std::string& origin,
                                            const std::string& referer,
                                            const provider::ProviderCallContext& context)
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
    
    auto [result, response] = sendWithinContext(
        context, "https://cube.tobit.cloud", request, chayns::kUpstreamUploadTimeoutSeconds);
    
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

platform::Result<provider::ProviderResponse> chaynsapi::doGenerate(
    const provider::ProviderRequest& request,
    provider::ProviderCallContext& context)
{
    if (const auto interrupted = interruptionError(context)) {
        return platform::Result<provider::ProviderResponse>::failure(*interrupted);
    }

    const Json::Value messageContext = historyForChayns(request.messages);
    LOG_INFO << "[chaynsAPI] 发送聊天消息";
    string modelname = request.model;

    // Resolve and validate the model before consuming an account or uploading
    // attachments. Unknown models get one forced catalog refresh so newly
    // published upstream models become usable without a process restart.
    chayns::ModelDescriptor selectedModel;
    if (!findModel(modelname, selectedModel)) {
        (void)loadModels(true, &context);
    }
    if (const auto interrupted = interruptionError(context)) {
        return platform::Result<provider::ProviderResponse>::failure(*interrupted);
    }
    if (!findModel(modelname, selectedModel)) {
        LOG_ERROR << "[chaynsAPI] 未找到请求模型: " << modelname;
        return chaynsFailure(platform::ErrorCode::BadRequest,
                             "Unknown model: " + modelname,
                             "model_not_found");
    }

    if (!request.images.empty()) {
        if (!chayns::supportsImageInput(selectedModel)) {
            LOG_WARN << "[chaynsAPI] 模型不支持图片输入: " << modelname;
            return chaynsFailure(platform::ErrorCode::BadRequest,
                                 "Model does not support image input",
                                 "unsupported_capability");
        }
        for (const auto& image : request.images) {
            if (!image.mediaType.empty() && !chayns::supportsMimeType(selectedModel, image.mediaType)) {
                LOG_WARN << "[chaynsAPI] 模型不支持附件类型: model=" << modelname
                         << ", mime=" << image.mediaType;
                return chaynsFailure(platform::ErrorCode::BadRequest,
                                     "Model does not support MIME type: " + image.mediaType,
                                     "unsupported_mime_type");
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
    std::shared_ptr<Accountinfo_st> selectedAccount;

    ThreadContext continuationContext;
    bool hasContinuationContext = false;
    if (!request.previousConversationId.empty()) {
        std::lock_guard<std::mutex> lock(m_threadMapMutex);
        auto it = m_threadMap.find(request.previousConversationId);
        if (it != m_threadMap.end()) {
            continuationContext = it->second;
            hasContinuationContext = true;
        }
    }
    
    std::unique_lock<std::mutex> accountExecutionLock;
    while (totalAttempts < MAX_UPSTREAM_RETRIES && !upstreamSuccess &&
           (!requestDeadlineStarted || m_clock->now() < requestDeadline)) {
        if (const auto interrupted = interruptionError(context)) {
            return platform::Result<provider::ProviderResponse>::failure(*interrupted);
        }
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
        std::shared_ptr<Accountinfo_st> previousAccount;
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
                m_accountSelector.getAccountByUserName(
                    "chaynsapi", continuationContext.accountUserName, selectedAccount);
                if (!isUsableChaynsAccount(selectedAccount, selectedModel.requiresPro)) {
                    LOG_WARN << "[chaynsAPI] 续聊账户不可用或权限不足: "
                             << continuationContext.accountUserName;
                    selectedAccount.reset();
                }
            }

            if (!selectedAccount && !m_accountSelector.getEligibleAccount(
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
                    return chaynsFailure(
                        platform::ErrorCode::ProviderError,
                        selectedModel.requiresPro
                            ? "No valid Pro account with workspaceUacId available for the requested model"
                            : "No valid Free account available for the requested model",
                        "account_unavailable", 503);
                }
            }
        }

        std::shared_ptr<Accountinfo_st> accountinfo = selectedAccount;
        const ChaynsRequestRoute requestRoute = requestRouteForAccount(*accountinfo);
        LOG_INFO << "[chaynsAPI] 已选择请求路由: accountType="
                 << accountinfo->accountType
                 << ", threadTypeId=" << requestRoute.threadTypeId
                 << ", workspaceUacIdConfigured="
                 << (requestRoute.workspaceUacId > 0);
        if (requestRoute.isPro && requestRoute.workspaceUacId <= 0) {
            LOG_ERROR << "[chaynsAPI] Pro 账号请求缺少 workspaceUacId 配置";
            return chaynsFailure(platform::ErrorCode::ProviderError,
                                 "Chayns Pro workspaceUacId is not configured",
                                 "chayns_pro_workspace_not_configured", 503);
        }

        if (accountExecutionLock.owns_lock()) {
            accountExecutionLock.unlock();
        }
        const auto accountGate = accountExecutionGate(accountinfo->userName);
        const auto accountWaitStartedAt = m_clock->now();
        accountExecutionLock = std::unique_lock<std::mutex>(*accountGate, std::defer_lock);
        while (!accountExecutionLock.try_lock()) {
            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<provider::ProviderResponse>::failure(*interrupted);
            }
            m_clock->sleepFor(std::chrono::milliseconds(20));
        }
        const auto accountWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            m_clock->now() - accountWaitStartedAt).count();
        LOG_INFO << "[chaynsAPI] 已获取账号单飞租约: waitMs=" << accountWaitMs;
        if (!requestDeadlineStarted) {
            requestDeadline = m_clock->now() +
                              chayns::kRequestPollingDeadline;
            requestDeadlineStarted = true;
        }
        

        if (accountinfo->personId.empty()) {
            LOG_INFO << "[chaynsAPI] personId为空，正在尝试获取";
            auto request = HttpRequest::newHttpRequest();
            request->setMethod(HttpMethod::Get);
            request->setPath("/v2/userSettings");
            request->addHeader("Authorization", "Bearer " + accountinfo->authToken);
            applyChaynsRouteHeaders(request, *accountinfo, requestRoute);
            auto [result, response] = sendWithinContext(
                context, "https://auth.chayns.net", request, chayns::kUpstreamRequestTimeoutSeconds);
            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<provider::ProviderResponse>::failure(*interrupted);
            }
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
                return chaynsFailure(platform::ErrorCode::ProviderError,
                                     "Failed to obtain a valid personId",
                                     "person_id_unavailable", 500);
            }
            m_clock->sleepFor(std::chrono::milliseconds(BASE_DELAY * 5));
            continue;
        }
        
        // ---- 3. 处理图片上传 (仅首次) ----
        if (!imagesUploaded && !request.images.empty()) {
            LOG_INFO << "[chaynsAPI] 正在处理" << request.images.size() << " 张图片上传";
            for (const auto& img : request.images) {
                std::string imageUrl = uploadImageToService(
                    img,
                    accountinfo->personId,
                    accountinfo->authToken,
                    accountinfo->userName,
                    requestRoute.origin,
                    requestRoute.referer,
                    context);
                if (const auto interrupted = interruptionError(context)) {
                    return platform::Result<provider::ProviderResponse>::failure(*interrupted);
                }
                if (!imageUrl.empty()) {
                    uploadedImageUrls.push_back(imageUrl);
                }
            }
            LOG_INFO << "[chaynsAPI] 成功上传" << uploadedImageUrls.size() << " 张图片";
            imagesUploaded = true;
        }
        
        // ---- 4. 发送请求（首次发送） ----
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
                     << ", previousProviderPresent=" << !request.previousConversationId.empty();
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
            string messageText = request.input;
            
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
            
            auto sendResult = sendWithinContext(
                context, "https://cube.tobit.cloud", reqSend, chayns::kUpstreamRequestTimeoutSeconds);
            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<provider::ProviderResponse>::failure(*interrupted);
            }
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
            LOG_INFO << "[chaynsAPI] 正在创建新线程： 注入系统提示词 (" << request.systemPrompt.length() << " 字符)";
            string full_message;
            const std::string historyHeader = "\n接下来是 OpenAI 接口格式的历史消息：\n";
            const std::string currentHeader = "\n用户现在的问题是:\n";
            const size_t fixedBytes = request.systemPrompt.size() +
                historyHeader.size() + currentHeader.size() + request.input.size();
            // chayns 的出站上限与其他 Provider 相互独立，走 outbound_limits.chaynsapi。
            const size_t chaynsRequestLimit =
                continuity::outboundMaxRequestBytes("chaynsapi");
            const size_t historyBudget = continuity::remainingHistoryBudget(
                chaynsRequestLimit,
                fixedBytes
            );
            const std::string currentHistoryMessage = request.rawInput.empty()
                ? request.input
                : request.rawInput;
            const auto historySelection = continuity::selectRecentHistory(
                messageContext,
                historyBudget,
                continuity::outboundMaxMessageBytes("chaynsapi"),
                false,
                currentHistoryMessage
            );
            const bool historyIncluded = !historySelection.messages.empty();

            if (historyIncluded) {
                Json::StreamWriterBuilder historyWriter;
                historyWriter["indentation"] = "";
                full_message = request.systemPrompt + historyHeader +
                    Json::writeString(historyWriter, historySelection.messages) +
                    currentHeader + request.input;
            } else {
                full_message = request.systemPrompt + "\n" + request.input;
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
            
            
            LOG_INFO << "[chaynsAPI] 正在创建新线程: threadTypeId="
                     << requestRoute.threadTypeId
                     << ", workspaceUacIdIncluded=" << requestRoute.isPro;
            
            // progressive degradation: full -> 1/2 -> 1/4 -> ... -> zero history
            const auto rebuildNewThreadText = [&](size_t budget) -> std::string {
                if (budget == 0) {
                    return request.systemPrompt + "\n" + request.input;
                }
                const auto trimmed = continuity::selectRecentHistory(
                    messageContext,
                    budget,
                    continuity::outboundMaxMessageBytes("chaynsapi"),
                    false,
                    currentHistoryMessage
                );
                if (trimmed.messages.empty()) {
                    return request.systemPrompt + "\n" + request.input;
                }
                Json::StreamWriterBuilder trimWriter;
                trimWriter["indentation"] = "";
                return request.systemPrompt + historyHeader +
                    Json::writeString(trimWriter, trimmed.messages) +
                    currentHeader + request.input;
            };

            const auto applyNewThreadText = [&](const std::string& text) {
                message["text"] = text;
                sendMessageRequest["messages"][0] = message;
            };

            const auto buildNewThreadRequest = [&]() {
                auto req = HttpRequest::newHttpJsonRequest(sendMessageRequest);
                req->setMethod(HttpMethod::Post);
                req->setPath("/intercom-backend/v2/thread?forceCreate=true");
                req->addHeader("Authorization", "Bearer " + accountinfo->authToken);
                applyChaynsRouteHeaders(req, *accountinfo, requestRoute);
                return req;
            };

            const auto ladder = continuity::degradationLadder(historyBudget);
            size_t ladderIndex = 0;

            // pre-send gate: shrink first instead of wasting a 413 round trip
            while (ladderIndex + 1 < ladder.size()) {
                const auto gate = continuity::checkOutboundSize("chaynsapi", sendMessageRequest);
                if (gate.withinLimit) break;
                ++ladderIndex;
                const std::string reduced = rebuildNewThreadText(ladder[ladderIndex]);
                LOG_WARN << "[chaynsAPI] new thread body exceeds outbound limit, pre-shrink: bodyBytes="
                         << gate.actualBytes << ", limit=" << gate.limitBytes
                         << ", nextHistoryBudget=" << ladder[ladderIndex]
                         << ", retryTextBytes=" << reduced.size();
                applyNewThreadText(reduced);
            }

            auto sendResult = sendWithinContext(
                context,
                "https://cube.tobit.cloud",
                buildNewThreadRequest(),
                chayns::kUpstreamRequestTimeoutSeconds);

            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<provider::ProviderResponse>::failure(*interrupted);
            }

            // 413 fallback: upstream hard limit may be lower than configured value
            while (sendResult.first == ReqResult::Ok && sendResult.second &&
                   static_cast<int>(sendResult.second->statusCode()) == 413 &&
                   ladderIndex + 1 < ladder.size()) {
                ++ladderIndex;
                const std::string reduced = rebuildNewThreadText(ladder[ladderIndex]);
                LOG_WARN << "[chaynsAPI] new thread got 413, degrade and retry: step="
                         << ladderIndex << "/" << (ladder.size() - 1)
                         << ", historyBudget=" << ladder[ladderIndex]
                         << ", originalBytes=" << full_message.size()
                         << ", retryBytes=" << reduced.size();
                applyNewThreadText(reduced);
                sendResult = sendWithinContext(
                    context,
                    "https://cube.tobit.cloud",
                    buildNewThreadRequest(),
                    chayns::kUpstreamRequestTimeoutSeconds);
                if (const auto interrupted = interruptionError(context)) {
                    return platform::Result<provider::ProviderResponse>::failure(*interrupted);
                }
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
            m_clock->sleepFor(std::chrono::milliseconds(BASE_DELAY * 5));
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
        const auto pollingStartedAt = m_clock->now();
        LOG_INFO << "[chaynsAPI] 开始轮询锚定消息，请求总截止时间: "
                 << std::chrono::duration_cast<std::chrono::seconds>(
                        chayns::kRequestPollingDeadline).count()
                 << " 秒";
        LOG_INFO << "[chaynsAPI] 轮询路径: " << pollPath+"&take=1000&viewMode=user&afterDate="+lastMessageTime;

        while (m_clock->now() < requestDeadline) {
            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<provider::ProviderResponse>::failure(*interrupted);
            }
            pollCount++;
            auto reqGet = HttpRequest::newHttpRequest();
            reqGet->setMethod(HttpMethod::Get);
            reqGet->setPath(pollPath);
            reqGet->setParameter("take", "1000");
            reqGet->setParameter("viewMode", "user");
            reqGet->setParameter("afterDate", lastMessageTime);
            reqGet->addHeader("Authorization", "Bearer " + accountinfo->authToken);
            applyChaynsRouteHeaders(reqGet, *accountinfo, requestRoute);

            auto getResult = sendWithinContext(
                context, "https://cube.tobit.cloud", reqGet, chayns::kUpstreamRequestTimeoutSeconds);
            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<provider::ProviderResponse>::failure(*interrupted);
            }
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

            const auto now = m_clock->now();
            if (now >= requestDeadline) {
                break;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - pollingStartedAt);
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                requestDeadline - now);
            m_clock->sleepFor(
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
        if (m_clock->now() < requestDeadline) {
            m_clock->sleepFor(std::chrono::milliseconds(BASE_DELAY * 10));
        }
        
    } // 外层重试循环结束（totalAttempts < MAX_UPSTREAM_RETRIES）

    if (!upstreamSuccess && m_clock->now() >= requestDeadline) {
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
            m_threadMap[request.conversationId] = ctx;
        }

        // 上游线程台账：内存 m_threadMap 会随进程退出/会话过期消失，
        // 但上游 thread 是真实存在的远端资源，必须单独留痕，否则重启即泄漏。
        {
            if (m_threadLedger && m_threadLedger->isEnabled() && !final_threadId.empty()) {
                chaynsThreadDbManager::ThreadRow row;
                row.threadId        = final_threadId;
                row.sessionId       = request.conversationId;
                row.accountUserName = final_accountUserName;
                row.origin          = final_origin;
                row.referer         = final_referer;
                row.createdAt       = static_cast<int64_t>(time(nullptr));
                row.lastActiveAt    = row.createdAt;
                // 异步落库：请求链路不等 DB，失败在 DbManager 内部降级忽略。
                m_threadLedger->asyncUpsertThread(row);
            }
        }

        provider::ProviderResponse response;
        response.text = std::move(final_response_message);
        response.meta.emplace("chayns.request_message_id", final_requestMessageId);
        response.meta.emplace("chayns.request_creation_time", final_requestCreationTime);
        response.meta.emplace("chayns.assistant_message_id", final_assistantMessageId);
        response.meta.emplace("chayns.reasoning_messages", compactJson(final_reasoningMessages));
        response.meta.emplace("chayns.account_type", final_accountType);
        response.meta.emplace("chayns.thread_type_id", std::to_string(final_threadTypeId));
        if (final_accountType == "pro") {
            response.meta.emplace("chayns.workspace_uac_id",
                                  std::to_string(final_workspaceUacId));
        }
        return platform::Result<provider::ProviderResponse>::success(std::move(response));
    }

    LOG_ERROR << "[chaynsAPI] 所有上游重试均失败 (总尝试次数：" << totalAttempts
              << "/" << MAX_UPSTREAM_RETRIES << ")";
    if (fatalAmbiguousSend || fatalCorrelationConflict || fatalResponseTimeout) {
        // A delayed upstream reply makes either local mapping unsafe for the
        // next turn; detach rather than issuing cleanup HTTP on this path.
        std::lock_guard<std::mutex> lock(m_threadMapMutex);
        if (!request.previousConversationId.empty()) {
            m_threadMap.erase(request.previousConversationId);
        }
        m_threadMap.erase(request.conversationId);
    }
    if (fatalAmbiguousSend || fatalCorrelationConflict || fatalResponseTimeout) {
        if (m_threadLedger && m_threadLedger->isEnabled()) {
            if (!request.previousConversationId.empty()) {
                m_threadLedger->asyncDetachThreadBySessionId(request.previousConversationId);
            }
            m_threadLedger->asyncDetachThreadBySessionId(request.conversationId);
        }
    }
    if (fatalCorrelationConflict) {
        return chaynsFailure(platform::ErrorCode::Conflict,
                             "Upstream thread contains an overlapping user message",
                             "upstream_message_conflict", 409);
    }
    if (fatalAmbiguousSend) {
        return chaynsFailure(platform::ErrorCode::ProviderError,
                             "Upstream message submission outcome is ambiguous",
                             "upstream_send_ambiguous", 502);
    }
    if (fatalResponseTimeout || context.deadlineExceeded()) {
        return chaynsFailure(platform::ErrorCode::Timeout,
                             "Timed out waiting for the anchored upstream response",
                             "upstream_response_timeout", 504);
    }
    return chaynsFailure(platform::ErrorCode::ProviderError,
                         "Upstream failed after all retries",
                         "upstream_retry_exhausted", 500);
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

bool chaynsapi::loadModels(
    bool forceRefresh,
    const provider::ProviderCallContext* context)
{
    if (!forceRefresh) {
        std::shared_lock<std::shared_mutex> lock(m_modelCatalogMutex);
        if (!m_modelCatalog.byName.empty() &&
            m_modelsLoadedAt.time_since_epoch().count() != 0 &&
            m_clock->now() - m_modelsLoadedAt < MODEL_CACHE_TTL) {
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
            m_clock->now() - m_modelsLoadedAt < MODEL_CACHE_TTL) {
            return true;
        }
    }

    const auto refreshStartedAt = m_clock->now();
    if (m_lastModelRefreshAttempt.time_since_epoch().count() != 0 &&
        refreshStartedAt - m_lastModelRefreshAttempt < MODEL_REFRESH_MIN_INTERVAL) {
        std::shared_lock<std::shared_mutex> lock(m_modelCatalogMutex);
        LOG_INFO << "[chaynsAPI] 模型目录刷新请求过于频繁，使用当前缓存";
        return !m_modelCatalog.byName.empty();
    }
    m_lastModelRefreshAttempt = refreshStartedAt;

    auto request = HttpRequest::newHttpRequest();
    
    request->setMethod(HttpMethod::Get);
    request->setPath("/chayns-ai-chatbot/nativeModelChatbot");
    chayns_browser::applyBrowserHeaders(request);
    
    if (context && interruptionError(*context)) {
        return false;
    }
    const auto exchange = context
        ? sendWithinContext(*context,
                            "https://cube.tobit.cloud",
                            request,
                            chayns::kUpstreamRequestTimeoutSeconds)
        : m_transport->send("https://cube.tobit.cloud",
                            request,
                            chayns::kUpstreamRequestTimeoutSeconds);
    const auto& result = exchange.first;
    const auto& response = exchange.second;
    if (context && interruptionError(*context)) {
        return false;
    }

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
        m_modelsLoadedAt = m_clock->now();
    }

    LOG_INFO << "[chaynsAPI] Native模型Chatbot模型加载成功: total=" << modelCount
             << ", pro=" << proModels
             << ", non_pro=" << (modelCount - proModels)
             << ", skipped=" << parsed.skipped
             << ", duplicates=" << parsed.duplicates;
    return true;
}

platform::Result<void> chaynsapi::transferThreadContext(
    const std::string& oldId,
    const std::string& newId)
{
    LOG_INFO << "[chaynsAPI] 正在尝试转移线程上下文，从" << oldId << " 到 " << newId;
    std::lock_guard<std::mutex> lock(m_threadMapMutex);
    const auto it = m_threadMap.find(oldId);
    if (it == m_threadMap.end()) {
        LOG_WARN << "[chaynsAPI] 转移线程上下文失败： oldId 未在线程Map中找到";
        return platform::Result<void>::success();
    }

    m_threadMap[newId] = it->second;
    m_threadMap.erase(it);
    LOG_INFO << "[chaynsAPI] 成功转移线程上下文，从" << oldId << " 到 " << newId;
    // 台账跟随轮转：否则旧 local key 的行会立刻显得"已过期"而被 reaper 误删活跃线程。
    if (m_threadLedger && m_threadLedger->isEnabled()) {
        m_threadLedger->asyncUpdateThreadSessionId(oldId, newId);
    }
    return platform::Result<void>::success();
}

platform::Result<void> chaynsapi::eraseThreadContext(
    const std::string& conversationId)
{
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(m_threadMapMutex);
        erased = m_threadMap.erase(conversationId) != 0;
    }
    LOG_INFO << "[chaynsAPI] 删除会话映射： convId 删除数量=" << (erased ? 1 : 0);

    // This path only detaches the ledger.  Reaper owns remote deletion, so a
    // local session cleanup never blocks on upstream HTTP.
    if (m_threadLedger && m_threadLedger->isEnabled()) {
        m_threadLedger->asyncDetachThreadBySessionId(conversationId);
    }
    return platform::Result<void>::success();
}

platform::Result<void> chaynsapi::deleteUpstreamThread(
    const std::string& accountUserName,
    const std::string& threadId,
    const std::string& origin,
    const std::string& referer)
{
    std::shared_ptr<Accountinfo_st> account;
    m_accountSelector.getAccountByUserName("chaynsapi", accountUserName, account);
    if (!account || account->authToken.empty() || account->personId.empty()) {
        LOG_WARN << "[chaynsAPI] 无法删除上游线程：账户不可用或缺少 personId";
        return platform::Result<void>::failure(platform::Error::providerError(
            "Unable to delete Chayns upstream thread", "thread_delete_account_unavailable"));
    }

    Json::Value body;
    Json::Value threadIds(Json::arrayValue);
    threadIds.append(threadId);
    body["threadIds"] = threadIds;
    body["personId"] = account->personId;

    auto request = HttpRequest::newHttpJsonRequest(body);
    request->setMethod(HttpMethod::Delete);
    request->setPath("/intercom-backend/v2/thread/member/delete");
    request->addHeader("Authorization", "Bearer " + account->authToken);
    chayns_browser::applyBrowserHeadersForAccount(
        request,
        account->userName,
        account->personId,
        origin.empty() ? CHAYNS_FREE_ORIGIN : origin,
        referer.empty() ? CHAYNS_FREE_REFERER : referer);

    auto [result, response] = m_transport->send(
        "https://cube.tobit.cloud", request, chayns::kUpstreamRequestTimeoutSeconds);
    if (result != ReqResult::Ok || !response ||
        (response->statusCode() != k200OK && response->statusCode() != k204NoContent)) {
        LOG_WARN << "[chaynsAPI] 删除上游线程失败, result=" << static_cast<int>(result)
                 << ", " << summarizeUpstreamResponse(response);
        const int status = response ? static_cast<int>(response->statusCode()) : 0;
        return platform::Result<void>::failure(platform::Error::providerError(
            "Failed to delete Chayns upstream thread", "thread_delete_failed", status));
    }
    LOG_INFO << "[chaynsAPI] 已删除上游线程";
    return platform::Result<void>::success();
}

ProviderModelCatalog chaynsapi::getModels()
{
    (void)loadModels(false);
    std::shared_lock<std::shared_mutex> lock(m_modelCatalogMutex);
    ProviderModelCatalog catalog;
    for (const auto& entry : m_modelCatalog.byName)
    {
        const auto& descriptor = entry.second;
        ProviderModel model;
        model.id = descriptor.id;
        model.created = 0;
        model.ownedBy = descriptor.developer.empty() ? "chayns" : descriptor.developer;
        ChaynsModelExtension extension;
        extension.personId = descriptor.personId;
        if (descriptor.usedModel > 0) extension.usedModel = descriptor.usedModel;
        if (descriptor.tobitId > 0) extension.tobitId = descriptor.tobitId;
        extension.requiresSidekickPro = descriptor.requiresPro;
        extension.capabilities.images = chayns::supportsImageInput(descriptor);
        extension.capabilities.imagesDeclared = descriptor.canHandleImages;
        extension.capabilities.functionCalling = descriptor.canHandleFunctionCalling;
        extension.capabilities.googleSearch = descriptor.canHandleGoogleSearch;
        extension.capabilities.thinking = descriptor.canUseThinking;
        extension.capabilities.supportedMimeTypes = descriptor.supportedMimeTypes;
        extension.skills = descriptor.skills;
        extension.knowledge = descriptor.knowledge;
        extension.developerName = descriptor.developer;
        extension.developerCountry = descriptor.developerCountry;
        extension.hostingProvider = descriptor.hostingProvider;
        extension.hostingCountry = descriptor.hostingCountry;
        extension.hostingInEurope = descriptor.hostingInEurope;
        if (descriptor.costIndicator >= 0) extension.costIndicator = descriptor.costIndicator;
        model.chayns = std::move(extension);
        catalog.models.push_back(std::move(model));
    }
    return catalog;
}
