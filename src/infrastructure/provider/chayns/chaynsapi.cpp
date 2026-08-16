#include <drogon/drogon.h>
#include <infrastructure/provider/chayns/ChaynsProvider.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <mutex>
#include <infrastructure/provider/limits/HistoryReplayBudget.h>
#include <infrastructure/provider/limits/OutboundBudget.h>
#include <string>
#include <string_view>
#include <stdexcept>
#include <set>
#include <infrastructure/provider/chayns/ChaynsBrowserImpersonation.h>
#include <infrastructure/provider/chayns/ChaynsProviderPolicy.h>
using std::string;

namespace {

constexpr auto MODEL_CACHE_TTL = std::chrono::minutes(15);
constexpr auto MODEL_REFRESH_MIN_INTERVAL = std::chrono::seconds(30);
constexpr std::size_t TOOL_BRIDGE_LOG_MAX_BYTES = 16U * 1024U;

constexpr std::string_view TOOL_INSTRUCTIONS_OPEN = "<tool_instructions>\n";
constexpr std::string_view TOOL_INSTRUCTIONS_CLOSE = "</tool_instructions>";
constexpr std::string_view TOOL_BRIDGE_LOG_TRUNCATION = "...<truncated>";

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

// The application wraps bridge definitions in this exact tag block before it
// reaches the provider.  Use the final generated opening tag so user content
// (including the explanatory empty tag in the wrapper) is never mistaken for
// provider-facing bridge instructions.
std::optional<std::string_view> toolBridgeInstructionsForLog(const std::string& input)
{
    const auto start = input.rfind(TOOL_INSTRUCTIONS_OPEN);
    if (start == std::string::npos) return std::nullopt;

    const auto end = input.rfind(TOOL_INSTRUCTIONS_CLOSE);
    if (end == std::string::npos || end < start + TOOL_INSTRUCTIONS_OPEN.size()) {
        return std::nullopt;
    }

    return std::string_view(input).substr(
        start, end + TOOL_INSTRUCTIONS_CLOSE.size() - start);
}

struct ToolBridgeLogPayload
{
    std::string text;
    std::size_t sourceBytes = 0;
    bool truncated = false;
};

std::string escapedLogByte(unsigned char value)
{
    switch (value) {
        case '\\': return "\\\\";
        case '\"': return "\\\"";
        case '\n': return "\\n";
        case '\r': return "\\r";
        case '\t': return "\\t";
        default: break;
    }

    if (value >= 0x20U && value != 0x7fU) {
        return std::string(1, static_cast<char>(value));
    }

    constexpr char hex[] = "0123456789ABCDEF";
    std::string escaped = "\\x00";
    escaped[2] = hex[(value >> 4U) & 0x0fU];
    escaped[3] = hex[value & 0x0fU];
    return escaped;
}

std::string escapeFullLogPayload(std::string_view source)
{
    std::string escaped;
    escaped.reserve(source.size());
    for (const unsigned char byte : source) {
        escaped += escapedLogByte(byte);
    }
    return escaped;
}

void logCompleteUserInputForDebug(std::string_view input,
                                  std::string_view requestId,
                                  std::string_view conversationId)
{
    // This is intentionally unbounded at DEBUG level so diagnostics can
    // distinguish an omitted user request from an upstream/model-side drift.
    // Escape control bytes to keep the file log one physical line per event.
    LOG_DEBUG << "[chaynsAPI][ToolBridge] 上游请求完整用户输入"
              << ": requestId=" << requestId
              << ", conversationId=" << conversationId
              << ", sourceBytes=" << input.size()
              << ", payload=\"" << escapeFullLogPayload(input) << "\"";
}

ToolBridgeLogPayload makeToolBridgeLogPayload(std::string_view source)
{
    ToolBridgeLogPayload payload;
    payload.sourceBytes = source.size();
    payload.text.reserve(std::min(source.size(), TOOL_BRIDGE_LOG_MAX_BYTES));

    const std::size_t textLimit = TOOL_BRIDGE_LOG_MAX_BYTES -
        TOOL_BRIDGE_LOG_TRUNCATION.size();
    for (const unsigned char byte : source) {
        const std::string escaped = escapedLogByte(byte);
        if (payload.text.size() + escaped.size() > textLimit) {
            payload.truncated = true;
            break;
        }
        payload.text += escaped;
    }
    if (payload.truncated) payload.text += TOOL_BRIDGE_LOG_TRUNCATION;
    return payload;
}

void logToolBridgePayload(const char* direction,
                          std::string_view source,
                          std::string_view requestId,
                          std::string_view conversationId)
{
    const ToolBridgeLogPayload payload = makeToolBridgeLogPayload(source);
    // Keep this single-line and bounded: Drogon's file logger then preserves
    // a searchable request/response pair without allowing response content to
    // forge additional log lines.
    LOG_INFO << "[chaynsAPI][ToolBridge] " << direction
             << ": requestId=" << requestId
             << ", conversationId=" << conversationId
             << ", sourceBytes=" << payload.sourceBytes
             << ", loggedBytes=" << payload.text.size()
             << ", truncated=" << payload.truncated;
    LOG_DEBUG << "[chaynsAPI][ToolBridge] " << direction
              << ": requestId=" << requestId
              << ", conversationId=" << conversationId
              << ", payload=\"" << payload.text << "\"";
}

}  // namespace

ChaynsProvider::ChaynsProvider(IAccountSelector& accountSelector,
                     std::shared_ptr<chayns::IChaynsHttpTransport> transport,
                     std::shared_ptr<chayns::IChaynsClock> clock,
                     std::shared_ptr<chayns::IChaynsThreadLedger> threadLedger,
                     FailureObserver failureObserver,
                     ChaynsProviderSettings settings)
    : ProviderBase(std::move(failureObserver)),
      m_accountSelector(accountSelector),
      m_protocolClient(
          std::make_shared<chayns::ChaynsProtocolClient>(std::move(transport))),
      m_clock(std::move(clock)),
      m_pollingLoop(m_protocolClient, m_clock),
      m_threadContext(std::move(threadLedger)),
      m_upstreamErrorTexts(std::move(settings.upstreamErrorTexts))
{
    if (!m_clock) {
        throw std::invalid_argument("chaynsapi requires a non-null clock");
    }
}

platform::Result<void> ChaynsProvider::initialize()
{
    if (!loadModels(true)) {
        return platform::Result<void>::failure(platform::Error::providerError(
            "Failed to load Chayns model catalog", "model_catalog_initialization"));
    }

    if (!m_upstreamErrorTexts.empty()) {
        LOG_INFO << "[chaynsAPI] 已从配置加载" << m_upstreamErrorTexts.size() << " 条上游错误文本";
    } else {
        LOG_WARN << "[chaynsAPI] 配置中未找到 upstream_error_texts，上游错误文本匹配将不可用";
    }
    return platform::Result<void>::success();
}

provider::ProviderCapabilities ChaynsProvider::capabilities() const noexcept
{
    return provider::ProviderCapabilities{/*nativeToolCalls=*/false,
                                          /*upstreamHistory=*/true,
                                          /*supportsImages=*/true};
}

std::shared_ptr<std::mutex> ChaynsProvider::accountExecutionGate(
    const std::string& accountUserName)
{
    std::lock_guard<std::mutex> lock(m_accountGatesMutex);
    auto& gate = m_accountGates[accountUserName];
    if (!gate) {
        gate = std::make_shared<std::mutex>();
    }
    return gate;
}

std::optional<platform::Error> ChaynsProvider::sleepWithinContext(
    const provider::ProviderCallContext& context,
    std::chrono::milliseconds duration) const
{
    if (const auto interrupted = interruptionError(context)) return interrupted;
    const auto bounded = std::min(duration, context.remaining());
    if (bounded <= std::chrono::milliseconds::zero()) {
        return platform::Error::timeout(
            "Chayns provider request deadline exceeded");
    }
    if (m_clock->sleepFor(
        bounded,
        [&context] {
            return context.isCancelled() || context.deadlineExceeded();
        })) {
        return std::nullopt;
    }
    if (const auto interrupted = interruptionError(context)) return interrupted;
    return platform::Error::timeout(
        "Chayns provider request deadline exceeded");
}

platform::Result<provider::ProviderResponse> ChaynsProvider::doGenerate(
    const provider::ProviderRequest& request,
    provider::ProviderCallContext& context)
{
    if (const auto interrupted = interruptionError(context)) {
        return platform::Result<provider::ProviderResponse>::failure(*interrupted);
    }

    const Json::Value messageContext = historyForChayns(request.messages);
    const auto toolBridgeInstructions = toolBridgeInstructionsForLog(request.input);
    logCompleteUserInputForDebug(request.input,
                                 request.requestId,
                                 request.conversationId);
    if (toolBridgeInstructions.has_value()) {
        // The INFO log remains limited to generated tool instructions.  The
        // complete request input is emitted separately at DEBUG level above.
        logToolBridgePayload("上游请求桥接内容", *toolBridgeInstructions,
                             request.requestId, request.conversationId);
    }
    LOG_INFO << "[chaynsAPI] 发送聊天消息: requestId=" << request.requestId
             << ", conversationId=" << request.conversationId;
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
    std::optional<platform::Error> lastSubmissionError;
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

    chayns::ThreadContext continuationContext;
    bool hasContinuationContext = false;
    std::optional<chayns::ThreadLedgerRow> incompleteLedgerContext;
    bool incompleteLedgerUsedFallbackKey = false;
    if (!request.previousConversationId.empty()) {
        const auto lookup = m_threadContext.lookup(
            request.previousConversationId, request.previousConversationFallbackId);
        if (lookup.context.has_value()) {
            continuationContext = *lookup.context;
            hasContinuationContext = true;
        }
        incompleteLedgerContext = lookup.incompleteLedgerContext;
        incompleteLedgerUsedFallbackKey = lookup.usedFallbackKey;
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

        if (!chayns::policy::isUsableAccount(selectedAccount, selectedModel.requiresPro)) {
            selectedAccount.reset();

            // A continuation may only reuse its original account when that
            // account is still valid and satisfies the current model.
            const std::string continuationAccount = hasContinuationContext
                ? continuationContext.accountUserName
                : (incompleteLedgerContext.has_value()
                       ? incompleteLedgerContext->accountUserName
                       : std::string{});
            if (totalAttempts == 1 && !needSwitchAccount && !continuationAccount.empty()) {
                m_accountSelector.getAccountByUserName(
                    "chaynsapi", continuationAccount, selectedAccount);
                if (!chayns::policy::isUsableAccount(selectedAccount, selectedModel.requiresPro)) {
                    LOG_WARN << "[chaynsAPI] 续聊账户不可用或权限不足: "
                             << continuationAccount;
                    selectedAccount.reset();
                }
            }

            if (!selectedAccount && !m_accountSelector.getEligibleAccount(
                    "chaynsapi", selectedAccount, accountRequirement, attemptedAccounts)) {
                // Switching is a retry preference, not an additional account
                // requirement. If there is only one eligible account, finish
                // the retry budget with it instead of misreporting a 503.
                if (chayns::policy::isUsableAccount(previousAccount, selectedModel.requiresPro)) {
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
        const chayns::policy::RequestRoute requestRoute =
            chayns::policy::requestRouteForAccount(*accountinfo);
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

        // Early ledger rows predate durable provider-context columns.  They
        // can be upgraded exactly once when the restored session also proves
        // that the requested model is unchanged.  Restrict compatibility to
        // the free route: a legacy row has no workspace ID, so reusing a Pro
        // thread could cross a changed workspace boundary.
        if (!hasContinuationContext && incompleteLedgerContext.has_value() &&
            totalAttempts == 1 && !needSwitchAccount) {
            const auto& legacy = *incompleteLedgerContext;
            const bool restoredModelMatches =
                !request.previousConversationFallbackModel.empty() &&
                request.previousConversationFallbackModel == modelname;
            const bool legacyFreeRouteMatches =
                !requestRoute.isPro && legacy.accountUserName == accountinfo->userName &&
                legacy.origin == requestRoute.origin && legacy.referer == requestRoute.referer;
            if (!legacy.threadId.empty() && legacyFreeRouteMatches && restoredModelMatches) {
                chayns::ThreadContext restored;
                restored.threadId = legacy.threadId;
                restored.userAuthorId = legacy.userAuthorId;
                restored.agentAuthorId = legacy.agentAuthorId;
                restored.accountUserName = accountinfo->userName;
                restored.modelId = modelname;
                restored.accountType = accountinfo->accountType;
                restored.threadTypeId = requestRoute.threadTypeId;
                restored.workspaceUacId = requestRoute.workspaceUacId;
                restored.origin = requestRoute.origin;
                restored.referer = requestRoute.referer;
                restored.lastRequestMessageId = legacy.lastRequestMessageId;
                restored.lastRequestCreationTime = legacy.lastRequestCreationTime;
                restored.lastAssistantMessageId = legacy.lastAssistantMessageId;
                continuationContext = restored;
                m_threadContext.cache(request.previousConversationId, std::move(restored));
                hasContinuationContext = true;
                LOG_INFO << "[chaynsAPI] 已兼容恢复旧版线程台账上下文: "
                         << "threadIdPresent=" << !continuationContext.threadId.empty()
                         << ", fallbackKeyUsed=" << incompleteLedgerUsedFallbackKey
                         << ", inserted=true";
            } else {
                LOG_WARN << "[chaynsAPI] 上游线程台账记录不完整，安全地创建新线程: "
                          << "threadIdPresent=" << !legacy.threadId.empty()
                         << ", accountMatches=" << (legacy.accountUserName == accountinfo->userName)
                         << ", routeMatches=" << legacyFreeRouteMatches
                         << ", restoredModelMatches=" << restoredModelMatches;
            }
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
            if (const auto interrupted =
                    sleepWithinContext(context, std::chrono::milliseconds(20))) {
                return platform::Result<provider::ProviderResponse>::failure(
                    *interrupted);
            }
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
            const auto personId = m_protocolClient->getPersonId(
                *accountinfo, requestRoute, context,
                request.requestId, request.conversationId);
            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<provider::ProviderResponse>::failure(*interrupted);
            }
            if (personId.has_value()) {
                accountinfo->personId = *personId;
                LOG_INFO << "[chaynsAPI] 成功获取 personId：personIdPresent=true";
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
            if (const auto interrupted = sleepWithinContext(
                    context, std::chrono::milliseconds(BASE_DELAY * 5))) {
                return platform::Result<provider::ProviderResponse>::failure(
                    *interrupted);
            }
            continue;
        }
        
        // ---- 3. 处理图片上传 (仅首次) ----
        if (!imagesUploaded && !request.images.empty()) {
            LOG_INFO << "[chaynsAPI] 正在处理" << request.images.size() << " 张图片上传";
            for (const auto& img : request.images) {
                std::string imageUrl = m_protocolClient->uploadImage(
                    img,
                    accountinfo->personId,
                    accountinfo->authToken,
                    accountinfo->userName,
                    requestRoute.origin,
                    requestRoute.referer,
                    context,
                    request.requestId,
                    request.conversationId);
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
            
            LOG_INFO << "[chaynsAPI] 正在发送后续消息到线程: threadIdPresent=" << !threadId.empty();

            const auto submission = m_protocolClient->sendFollowupMessage(
                threadId, messageBody, *accountinfo, requestRoute, context,
                request.requestId, request.conversationId);
            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<provider::ProviderResponse>::failure(*interrupted);
            }
            if (!submission.accepted) {
                LOG_ERROR << "[chaynsAPI] 发送后续消息失败: status="
                          << submission.statusCode;
                if (submission.error.hasError()) {
                    lastSubmissionError = submission.error;
                }
                sendFailed = true;
                fatalAmbiguousSend = submission.ambiguous;
            } else {
                lastMessageTime = submission.creationTime;
                requestMessageId = submission.messageId;
                userAuthorId = submission.userAuthorId;
            }
        } else {
            // =================================================
            // 分支 B： 新对话 (创建新 线程)
            // =================================================
            LOG_INFO << "[chaynsAPI] 本次创建新线程： 注入系统提示词 (" << request.systemPrompt.length() << " 字符)";
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

            auto submission = m_protocolClient->createThread(
                sendMessageRequest, *accountinfo, requestRoute,
                accountinfo->personId, selectedModel.personId, context,
                request.requestId, request.conversationId);

            if (const auto interrupted = interruptionError(context)) {
                return platform::Result<provider::ProviderResponse>::failure(*interrupted);
            }

            // 413 fallback: upstream hard limit may be lower than configured value
            while (submission.statusCode == 413 &&
                   ladderIndex + 1 < ladder.size()) {
                ++ladderIndex;
                const std::string reduced = rebuildNewThreadText(ladder[ladderIndex]);
                LOG_WARN << "[chaynsAPI] new thread got 413, degrade and retry: step="
                         << ladderIndex << "/" << (ladder.size() - 1)
                         << ", historyBudget=" << ladder[ladderIndex]
                         << ", originalBytes=" << full_message.size()
                         << ", retryBytes=" << reduced.size();
                applyNewThreadText(reduced);
                submission = m_protocolClient->createThread(
                    sendMessageRequest, *accountinfo, requestRoute,
                    accountinfo->personId, selectedModel.personId, context,
                    request.requestId, request.conversationId);
                if (const auto interrupted = interruptionError(context)) {
                    return platform::Result<provider::ProviderResponse>::failure(*interrupted);
                }
            }

            if (!submission.accepted) {
                LOG_ERROR << "[chaynsAPI] 创建线程失败: status="
                          << submission.statusCode;
                if (submission.error.hasError()) {
                    lastSubmissionError = submission.error;
                }
                sendFailed = true;
                fatalAmbiguousSend = submission.ambiguous;
            } else {
                threadId = submission.threadId;
                userAuthorId = submission.userAuthorId;
                agentAuthorId = submission.agentAuthorId;
                lastMessageTime = submission.creationTime;
                requestMessageId = submission.messageId;
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
            if (const auto interrupted = sleepWithinContext(
                    context, std::chrono::milliseconds(BASE_DELAY * 5))) {
                return platform::Result<provider::ProviderResponse>::failure(
                    *interrupted);
            }
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
        auto polling = m_pollingLoop.poll(
            messageAnchor, *accountinfo, requestRoute, requestDeadline, context,
            request.requestId, request.conversationId);
        if (!polling) {
            return platform::Result<provider::ProviderResponse>::failure(polling.error());
        }
        auto pollResult = std::move(polling.value());
        Json::Value reasoningMessages = std::move(pollResult.reasoningMessages);
        string response_message = std::move(pollResult.responseMessage);
        const int response_statusCode = pollResult.statusCode;
        fatalCorrelationConflict = pollResult.correlationConflict;
        fatalResponseTimeout = !pollResult.found;
        final_assistantMessageId = std::move(pollResult.assistantMessageId);
        messageAnchor.agentAuthorId = std::move(pollResult.agentAuthorId);

        if (response_statusCode == 200) {
            LOG_INFO << "[chaynsAPI] 回复已接收: textLength="
                     << response_message.size();
            if (toolBridgeInstructions.has_value()) {
                logToolBridgePayload("上游响应桥接内容",
                                     response_message,
                                     request.requestId,
                                     request.conversationId);
            }
            LOG_DEBUG << "[chaynsAPI] 回复内容: " << response_message;
        } else if (fatalCorrelationConflict) {
            LOG_ERROR << "[chaynsAPI] 当前消息最终回复前出现另一条用户消息，拒绝猜测回复归属";
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
            const auto requestRemaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    requestDeadline - m_clock->now());
            if (const auto interrupted = sleepWithinContext(
                    context,
                    std::min(std::chrono::milliseconds(BASE_DELAY * 10),
                             requestRemaining))) {
                return platform::Result<provider::ProviderResponse>::failure(
                    *interrupted);
            }
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
        chayns::ThreadContext contextToStore;
        contextToStore.threadId = final_threadId;
        contextToStore.userAuthorId = final_userAuthorId;
        contextToStore.agentAuthorId = final_agentAuthorId;
        contextToStore.accountUserName = final_accountUserName;
        contextToStore.modelId = modelname;
        contextToStore.accountType = final_accountType;
        contextToStore.threadTypeId = final_threadTypeId;
        contextToStore.workspaceUacId = final_workspaceUacId;
        contextToStore.origin = final_origin;
        contextToStore.referer = final_referer;
        contextToStore.lastRequestMessageId = final_requestMessageId;
        contextToStore.lastRequestCreationTime = final_requestCreationTime;
        contextToStore.lastAssistantMessageId = final_assistantMessageId;
        // The context component owns both the process-local mapping and the
        // credential-free durable ledger update.
        m_threadContext.store(request.conversationId, contextToStore);

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
        std::vector<std::string> detachedConversationIds;
        if (!request.previousConversationId.empty()) {
            detachedConversationIds.push_back(request.previousConversationId);
        }
        if (!request.previousConversationFallbackId.empty() &&
            request.previousConversationFallbackId != request.previousConversationId) {
            detachedConversationIds.push_back(request.previousConversationFallbackId);
        }
        detachedConversationIds.push_back(request.conversationId);
        m_threadContext.detach(detachedConversationIds);
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
    if (lastSubmissionError.has_value()) {
        return platform::Result<provider::ProviderResponse>::failure(
            std::move(*lastSubmissionError));
    }
    return chaynsFailure(platform::ErrorCode::ProviderError,
                         "Upstream failed after all retries",
                         "upstream_retry_exhausted", 500);
}
bool ChaynsProvider::findModel(const std::string& modelName, chayns::ModelDescriptor& model) const
{
    std::shared_lock<std::shared_mutex> lock(m_modelCatalogMutex);
    const auto it = m_modelCatalog.byName.find(modelName);
    if (it == m_modelCatalog.byName.end()) {
        return false;
    }
    model = it->second;
    return true;
}

bool ChaynsProvider::loadModels(
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

    if (context && interruptionError(*context)) {
        return false;
    }
    const auto apiModels = m_protocolClient->getModelCatalog(context);
    if (context && interruptionError(*context)) {
        return false;
    }
    if (!apiModels.has_value()) return false;

    auto parsed = chayns::parseModelCatalog(*apiModels);
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

platform::Result<void> ChaynsProvider::transferThreadContext(
    const std::string& oldId,
    const std::string& newId)
{
    return m_threadContext.transfer(oldId, newId);
}

platform::Result<void> ChaynsProvider::eraseThreadContext(
    const std::string& conversationId)
{
    // This path only detaches the ledger.  Reaper owns remote deletion, so a
    // local session cleanup never blocks on upstream HTTP.
    return m_threadContext.erase(conversationId);
}

platform::Result<void> ChaynsProvider::deleteUpstreamThread(
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

    const auto result = m_protocolClient->deleteThread(
        *account, threadId, origin, referer);
    if (!result) return result;
    LOG_INFO << "[chaynsAPI] 已删除上游线程";
    return platform::Result<void>::success();
}

ProviderModelCatalog ChaynsProvider::getModels()
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

std::optional<ProviderModelCapabilities> ChaynsProvider::findModelCapabilities(
    const std::string& modelId) const
{
    chayns::ModelDescriptor descriptor;
    if (!findModel(modelId, descriptor)) return std::nullopt;

    ProviderModelCapabilities capabilities;
    capabilities.images = chayns::supportsImageInput(descriptor);
    capabilities.imagesDeclared = descriptor.canHandleImages;
    capabilities.functionCalling = descriptor.canHandleFunctionCalling;
    capabilities.googleSearch = descriptor.canHandleGoogleSearch;
    capabilities.thinking = descriptor.canUseThinking;
    capabilities.supportedMimeTypes = descriptor.supportedMimeTypes;
    return capabilities;
}
