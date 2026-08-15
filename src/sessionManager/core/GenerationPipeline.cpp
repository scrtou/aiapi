#include <sessionManager/core/GenerationPipeline.h>
#include <sessionManager/continuity/ContinuityResolver.h>
#include <sessionManager/continuity/ResponseIndex.h>
#include <sessionManager/tooling/BridgeHelpers.h>
#include <sessionManager/tooling/ToolDefinitionEncoder.h>
#include <sessionManager/tooling/BridgeProtocolCodec.h>
#include <sessionManager/actionProtocol/ActionProtocolCompiler.h>
#include <json/json.h>
#include <platform/Log.h>
#include <platform/Uuid.h>

#include <chrono>
#include <exception>
#include <sstream>
#include <string_view>
#include <utility>

#include <sessionManager/core/Session.h>

using namespace provider;
using namespace session;
using namespace bridge;

namespace {
Json::StreamWriterBuilder& compactJsonWriter() {
    static thread_local Json::StreamWriterBuilder writer = [] {
        Json::StreamWriterBuilder instance;
        instance["indentation"] = "";
        return instance;
    }();
    return writer;
}

std::string toCompactJson(const Json::Value& value) {
    return Json::writeString(compactJsonWriter(), value);
}

constexpr auto kProviderCallBudget = std::chrono::minutes(5);
} // 匿名命名空间

generation::GenerationPipeline::GenerationPipeline(
    IProviderRegistry* providerRegistry,
    chatSession* sessionStore,
    IResponseIndex* responseIndex,
    session::IExecutionGate* executionGate,
    IChannelCatalog* channelCatalog,
    Json::Value runtimeConfig)
    : providerRegistry_(providerRegistry),
      sessionStore_(sessionStore),
      responseIndex_(responseIndex),
      executionGate_(executionGate),
      runtimeConfig_(std::move(runtimeConfig)),
      responsePipeline_(sessionStore, channelCatalog, runtimeConfig_)
{
}

std::string generation::GenerationPipeline::executionKey(const session_st& session) {
    // 门控键统一使用会话ID（会话..conversationId），确保同一会话并发策略一致
    return session.state.conversationId;
}

/**
 * @brief 将控制层统一请求对象物化为会话执行对象
 *
 * 【字段映射详细说明】
 * - request.*: 写入模型、通道、工具选择、原始输入等执行入参。
 * - provider.*: 写入客户端信息，用于后续输出清洗与兼容策略。
 * - state/时间字段: 初始化创建时间与活跃时间，供会话管理与淘汰逻辑使用。
 * - messageContext: 将强类型消息历史统一转为 Json 结构，保持旧链路兼容。
 */
session_st generation::GenerationPipeline::materializeRequest(const GenerationRequest& req) {
    LOG_INFO << "[生成服务] 开始将生成请求物化为会话结构";
    
    session_st session;
    
    // 基本参数映射
    session.request.model = req.model;
    session.request.api = req.provider.empty() ? "chaynsapi" : req.provider;
    session.request.systemPrompt = req.systemPrompt;
    session.provider.clientInfo = req.clientInfo;
    session.request.message = req.currentInput;
    session.request.images = req.images;  // 传递图片列表
    session.request.tools = req.tools;           // 传递规范化后的工具定义
    session.request.toolsRaw = (!req.toolsRaw.isNull() && req.toolsRaw.isArray())
        ? req.toolsRaw
        : req.tools;                             // 保留客户端原始工具定义
    session.request.toolChoice = req.toolChoice; // 传递工具选择策略
    session.request.parallelToolCalls = req.parallelToolCalls;
    session.request.rawMessage = req.currentInput; // 保留原始输入（工具桥接注入前）
    session.state.requestId = req.requestId.empty()
        ? ("req_" + platform::generateUuidV4())
        : req.requestId;
    session.state.lastActiveAt = time(nullptr);
    session.state.createdAt = time(nullptr);
    
    // 协议类型：统一映射为 ApiType 枚举，便于后续分支处理
    session.state.apiType = req.isResponseApi() ? ApiType::Responses : ApiType::ChatCompletions;
    session.state.hasPreviousResponseId = req.previousResponseId.has_value() &&
                                      !req.previousResponseId->empty();
    // prev_上游_key / conversationId 由会话连续性决策器与 会话Store 在后续阶段赋值，避免物化阶段写入不稳定状态
    
    
    // 转换消息上下文：将强类型消息序列化为 JSON，保持与既有处理链兼容
    for (const auto& msg : req.messages) {
        Json::Value jsonMsg;
        switch (msg.role) {
            case MessageRole::User:
                jsonMsg["role"] = "user";
                break;
            case MessageRole::Assistant:
                jsonMsg["role"] = "assistant";
                break;
            case MessageRole::System:
                jsonMsg["role"] = "system";
                break;
            case MessageRole::Tool:
                jsonMsg["role"] = "tool";
                break;
        }
        jsonMsg["content"] = msg.getTextContent();
        if (!msg.toolCalls.empty()) {
            jsonMsg["tool_calls"] = Json::Value(Json::arrayValue);
            for (const auto& toolCall : msg.toolCalls) {
                jsonMsg["tool_calls"].append(toolCall);
            }
        }
        if (!msg.toolCallId.empty()) {
            jsonMsg["tool_call_id"] = msg.toolCallId;
        }
        session.addMessageToContext(jsonMsg);
    }
    
    LOG_INFO << "[生成服务] 物化完成，模型: " << session.request.model
             << ", 协议类型: " << (session.isResponseApi() ? "Responses" : "ChatCompletions")
             << ", 上下文消息数: " << session.provider.messageContext.size();
    
    return session;
}

/**
 * @brief 在执行门控保护下完成一次完整生成流程
 *
 * 【执行门控主流程详细说明】
 * 1. 计算会话门控键并尝试获取执行权限。
 * 2. 在门控范围内依次执行：能力检查、上游调用、结果事件发送、会话提交。
 * 3. 在调用上游前后都检查取消状态，确保取消请求可及时生效。
 * 4. 无论成功/失败/异常，最终都由统一出口关闭 sink，并依赖 RAII 自动释放门控。
 */
namespace {

void resetRequestScopedBridgeState(session_st& session)
{
    session.provider.toolBridgeTrigger.clear();
    session.provider.toolBridgeFormat = toolcall::BridgeWireFormat::Unset;
    session.provider.toolBridgeAllowFormatFallback = false;
}

void bindResponseId(session_st& session, IResponseIndex& responseIndex)
{
    if (session.isResponseApi() && session.response.responseId.empty()) {
        session.response.responseId = chatSession::generateResponseId();
    }
    if (session.isResponseApi() && !session.response.responseId.empty()) {
        responseIndex.bind(session.response.responseId, session.state.conversationId);
    }
}

void emitStartedEvent(const session_st& session, IResponseSink& sink)
{
    generation::Started started;
    started.responseId = session.isResponseApi()
        ? session.response.responseId
        : session.state.conversationId;
    started.model = session.request.model;
    sink.onEvent(started);
}

std::optional<platform::Error> emitCancellation(
    const session_st& session,
    const std::string& logMessage,
    const std::string& metricMessage,
    generation::GenerationResponsePipeline& responsePipeline,
    IResponseSink& sink)
{
    LOG_INFO << "[生成服务] " << logMessage;
    recordWarnStat(session, metrics::Domain::SESSION_GATE,
                   metrics::EventType::SESSIONGATE_CANCELLED, metricMessage);
    responsePipeline.emitError(platform::ErrorCode::Cancelled, "请求已取消", sink);
    sink.onClose();
    return platform::Error::cancelled("请求已取消");
}

void prepareNextSessionId(chatSession& sessionStore, session_st& session)
{
    if (continuity::hasStableClientSession(session.provider.clientInfo)) {
        session.state.nextSessionId = session.state.conversationId;
    } else {
        sessionStore.prepareNextSessionId(session);
    }
}

void persistCompletedSession(chatSession& sessionStore,
                             IResponseIndex& responseIndex,
                             session_st& session)
{
    if (session.isResponseApi() && !session.response.responseId.empty()) {
        session.response.lastResponseId = session.response.responseId;
    }
    sessionStore.coverSessionresponse(session);
    if (session.isResponseApi() && !session.response.responseId.empty()) {
        responseIndex.bind(session.response.responseId, session.state.conversationId);
    }
}

} // namespace

generation::GenerationPipeline::ToolBridgeState
    generation::GenerationPipeline::prepareToolBridge(session_st& session) const
{
    ToolBridgeState state;
    state.supportsToolCalls = responsePipeline_.channelSupportsToolCalls(session.request.api);
    LOG_INFO << "[生成服务] 工具能力检查: 是否支持原生工具调用=" << state.supportsToolCalls
             << "，tools 是否为空=" << session.request.tools.isNull()
             << "，tools 是否数组=" << session.request.tools.isArray()
             << "，tools 数量=" << session.request.tools.size();

    const Json::Value toolsForBridge =
        (!session.request.tools.isNull() && session.request.tools.isArray() &&
         session.request.tools.size() > 0)
            ? session.request.tools
            : session.request.toolsRaw;
    state.hasToolDefinitions = toolsForBridge.isArray() && toolsForBridge.size() > 0;
    state.toolChoiceNone = toLowerStr(session.request.toolChoice) == "none";
    if (state.supportsToolCalls || state.toolChoiceNone || !state.hasToolDefinitions) {
        return state;
    }

    LOG_INFO << "[生成服务] 通道不支持原生工具调用，已注入工具桥接提示到请求内容";
    if (session.request.tools.isNull() || !session.request.tools.isArray() ||
        session.request.tools.size() == 0) {
        session.request.tools = toolsForBridge;
    }
    toolcall::transformRequestForToolBridge(session, runtimeConfig_);
    return state;
}

void generation::GenerationPipeline::retryCodexBridgeResponse(
    session_st& session,
    const ToolBridgeState& bridge,
    const platform::CancellationToken& cancellation,
    platform::Deadline deadline)
{
    const std::string clientType = safeJsonAsString(
        session.provider.clientInfo.get("client_type", ""), "");
    const bool shouldRetry =
        actionproto::capabilitiesForClient(clientType, session.request.parallelToolCalls).family ==
            actionproto::ClientFamily::Codex &&
        !bridge.supportsToolCalls && !bridge.toolChoiceNone &&
        !session.provider.toolBridgeTrigger.empty() && bridge.hasToolDefinitions;
    if (!shouldRetry) return;

    const std::string firstText = safeJsonAsString(
        session.response.message.get("message", ""), "");
    const bool hasNativeCalls = session.response.message.isMember("tool_calls") &&
        session.response.message["tool_calls"].isArray() &&
        session.response.message["tool_calls"].size() > 0;
    bool hasActionProtocol = false;
    const size_t sentinel = firstText.find(session.provider.toolBridgeTrigger);
    if (sentinel != std::string::npos) {
        const std::string candidate = firstText.substr(sentinel);
        toolcall::BridgePolicyOptions options;
        options.clientType = clientType;
        options.channel = session.request.api;
        options.model = session.request.model;
        options.sentinel = session.provider.toolBridgeTrigger;
        options.parallelToolCalls = session.request.parallelToolCalls;
        auto decoded = toolcall::createBridgeProtocolCodec(session.provider.toolBridgeFormat)
            ->decodeResponse(candidate, options);
        if ((!decoded.matched || !decoded.valid) &&
            session.provider.toolBridgeAllowFormatFallback) {
            const auto fallback = session.provider.toolBridgeFormat == toolcall::BridgeWireFormat::Json
                ? toolcall::BridgeWireFormat::Xml
                : toolcall::BridgeWireFormat::Json;
            decoded = toolcall::createBridgeProtocolCodec(fallback)->decodeResponse(candidate, options);
        }
        hasActionProtocol = decoded.valid;
        if (decoded.matched && !decoded.valid) {
            LOG_WARN << "[生成服务][CodexRooCompat] 首次动作协议无效: format="
                     << toolcall::bridgeWireFormatName(session.provider.toolBridgeFormat)
                     << ", error=" << decoded.diagnostic.message;
        }
    }
    if (hasNativeCalls || hasActionProtocol) return;

    LOG_WARN << "[生成服务][CodexRooCompat] 首次响应缺少有效 action protocol，正在严格重试一次";
    const std::string bridgeMessage = session.request.message;
    const Json::Value firstResponse = session.response.message;
    toolcall::BridgePolicyOptions options;
    options.clientType = clientType;
    options.channel = session.request.api;
    options.model = session.request.model;
    options.sentinel = session.provider.toolBridgeTrigger;
    options.parallelToolCalls = session.request.parallelToolCalls;
    const auto codec = toolcall::createBridgeProtocolCodec(session.provider.toolBridgeFormat);
    session.request.message += "\n\n[CodexRooCompat retry]\n"
        "Your previous response was invalid for this transport.\n" +
        codec->buildRetryPrompt(options);
    session.response.message = Json::Value(Json::objectValue);

    const auto retryError = invokeProvider(session, cancellation, deadline);
    session.request.message = bridgeMessage;
    if (retryError) {
        session.response.message = firstResponse;
        LOG_WARN << "[生成服务][CodexRooCompat] 严格重试失败，已恢复首次响应: code="
                 << retryError->type();
    } else {
        LOG_INFO << "[生成服务][CodexRooCompat] 严格重试完成";
    }
}

std::optional<platform::Error> generation::GenerationPipeline::execute(
    session_st& session,
    IResponseSink& sink,
    bool stream,
    ConcurrencyPolicy policy)
{
    (void)stream;
    const std::string sessionKey = executionKey(session);
    LOG_INFO << "[生成服务] 执行门控, 会话密钥: " << sessionKey
             << ", 策略: " << (policy == ConcurrencyPolicy::RejectConcurrent
                 ? "拒绝并发" : "取消前一个");
    if (!executionGate_ || !sessionStore_ || !responseIndex_) {
        return platform::Error::internal("生成服务依赖未注入");
    }

    ExecutionGuard guard(*executionGate_, sessionKey, policy);
    if (!guard.isAcquired()) {
        if (guard.getResult() == GateResult::Rejected) {
            LOG_WARN << "[生成服务] 因并发执行被拒绝, 会话密钥: " << sessionKey;
            recordErrorStat(session, metrics::Domain::SESSION_GATE,
                            metrics::EventType::SESSIONGATE_REJECTED_CONFLICT,
                            "并发冲突，请求被拒绝");
            return platform::Error::conflict("当前会话已有进行中的请求，请稍后重试");
        }
        LOG_ERROR << "[生成服务] 意外的门控结果: "
                  << static_cast<int>(guard.getResult());
        return platform::Error::internal("获取执行门控失败");
    }

    const auto cancellation = guard.cancellationToken();
    const platform::Deadline deadline = std::chrono::steady_clock::now() + kProviderCallBudget;
    try {
        auto& sessionStore = *sessionStore_;
        resetRequestScopedBridgeState(session);
        const ToolBridgeState bridge = prepareToolBridge(session);
        bindResponseId(session, *responseIndex_);
        emitStartedEvent(session, sink);

        if (guard.isCancelled()) {
            return emitCancellation(session, "调用提供者前请求被取消", "调用上游前请求已被取消",
                                    responsePipeline_, sink);
        }
        if (const auto error = invokeProvider(session, cancellation, deadline)) {
            const int status = error->httpStatus();
            recordErrorStat(session, metrics::Domain::UPSTREAM,
                            metrics::EventType::UPSTREAM_HTTP_ERROR,
                            error->message, status);
            responsePipeline_.emitError(*error, sink);
            sink.onClose();
            recordRequestCompletedStat(session, status);
            return std::nullopt;
        }

        retryCodexBridgeResponse(session, bridge, cancellation, deadline);
        if (guard.isCancelled()) {
            return emitCancellation(session, "调用提供者后请求被取消", "请求已取消 after provider call",
                                    responsePipeline_, sink);
        }

        prepareNextSessionId(sessionStore, session);
        responsePipeline_.emit(session, sink);
        persistCompletedSession(sessionStore, *responseIndex_, session);
        recordRequestCompletedStat(session, 200);
    } catch (const std::exception& error) {
        LOG_ERROR << "[生成服务] 执行门控会话异常: " << error.what();
        recordErrorStat(session, metrics::Domain::INTERNAL,
                        metrics::EventType::INTERNAL_EXCEPTION, error.what(), 500);
        responsePipeline_.emitError(platform::ErrorCode::Internal, error.what(), sink);
    } catch (...) {
        LOG_ERROR << "[生成服务] 执行门控会话未知异常";
        recordErrorStat(session, metrics::Domain::INTERNAL,
                        metrics::EventType::INTERNAL_UNKNOWN, "发生未知错误", 500);
        responsePipeline_.emitError(platform::ErrorCode::Internal, "发生未知错误", sink);
    }

    sink.onClose();
    return std::nullopt;
}

// ========== 新主入口实现（ 统一调用） ==========

std::optional<platform::Error> generation::GenerationPipeline::run(
    const GenerationRequest& req,
    IResponseSink& sink,
    ConcurrencyPolicy policy
) {
    LOG_INFO << "[生成服务] 进入 GenerationPipeline 主入口，协议："
             << (req.isResponseApi() ? "Responses" : "ChatCompletions")
             << ", 流式: " << req.stream;

    if (!sessionStore_ || !responseIndex_) {
        return platform::Error::internal("生成 pipeline 连续性依赖未注入");
    }

    // 1. 物化请求：Generation请求 → 会话_st
    session_st session = materializeRequest(req);
    
    // 2. 解析 会话Id 并 getOr创建（必须先于门控）
    auto& sessionManager = *sessionStore_;

    ContinuityResolver resolver(*responseIndex_, sessionManager.getTrackingMode());
    const ContinuityDecision decision = resolver.resolve(req);

    LOG_INFO << "[生成服务] 连续性决策"
             << " 来源=" << static_cast<int>(decision.source)
             << " 模式=" << (decision.mode == SessionTrackingMode::ZeroWidth ? "ZeroWidth" : "Hash")
             << " 会话ID=" << decision.sessionId
             << (decision.debug.empty() ? "" : (" 调试信息=" + decision.debug));

    const std::string currentRequestId = session.state.requestId;
    sessionManager.getOrCreateSession(decision.sessionId, session);
    // Session continuation restores persisted state. requestId is request
    // scoped and must never be inherited from the previous turn.
    session.state.requestId = currentRequestId;

    LOG_INFO << "[生成服务] 会话 " << (session.state.isContinuation ? "续接" : "新建")
             << ", 会话ID: " << session.state.conversationId
             << ", 协议类型: " << (session.isResponseApi() ? "Responses" : "ChatCompletions");
    
    // 3. 调用共享执行函数 executeGuardedWith会话()
    return execute(session, sink, req.stream, policy);
}

provider::ProviderRequest generation::GenerationPipeline::providerRequestFromSession(
    const session_st& session)
{
    provider::ProviderRequest request;
    request.conversationId = session.state.conversationId;
    if (session.state.isContinuation) {
        request.previousConversationId = session.provider.prevProviderKey;
    }
    request.model = session.request.model;
    request.systemPrompt = session.request.systemPrompt;
    request.input = session.request.message;
    request.rawInput = session.request.rawMessage;
    request.images = session.request.images;
    request.toolChoice = session.request.toolChoice;
    request.parallelToolCalls = session.request.parallelToolCalls;
    request.requestId = session.state.requestId;

    if (session.provider.clientInfo.isObject()) {
        for (const char* key : {"workspace_id", "workspaceId"}) {
            const auto& value = session.provider.clientInfo[key];
            if (value.isString() && !value.asString().empty()) {
                request.routingHints.emplace(key, value.asString());
            }
        }
    }

    if (session.provider.messageContext.isArray()) {
        request.messages.reserve(session.provider.messageContext.size());
        for (const auto& legacyMessage : session.provider.messageContext) {
            provider::ProviderMessage message;
            const std::string role = legacyMessage.get("role", "user").asString();
            if (role == "system") {
                message.role = provider::ProviderMessageRole::System;
            } else if (role == "assistant") {
                message.role = provider::ProviderMessageRole::Assistant;
            } else if (role == "tool") {
                message.role = provider::ProviderMessageRole::Tool;
            } else {
                message.role = provider::ProviderMessageRole::User;
            }
            message.text = legacyMessage.get("content", "").asString();
            message.toolCallId = legacyMessage.get("tool_call_id", "").asString();
            request.messages.push_back(std::move(message));
        }
    }

    const Json::Value& tools = (!session.request.tools.isNull() && session.request.tools.isArray())
        ? session.request.tools
        : session.request.toolsRaw;
    if (tools.isArray()) {
        request.tools.reserve(tools.size());
        for (const auto& item : tools) {
            const Json::Value& function = item.isMember("function") && item["function"].isObject()
                ? item["function"]
                : item;
            provider::ProviderToolDefinition definition;
            definition.name = function.get("name", "").asString();
            definition.description = function.get("description", "").asString();
            if (function.isMember("parameters")) {
                definition.parametersJson = toCompactJson(function["parameters"]);
            }
            if (!definition.name.empty()) {
                request.tools.push_back(std::move(definition));
            }
        }
    }
    return request;
}

void generation::GenerationPipeline::applyProviderResponse(
    session_st& session,
    const provider::ProviderResponse& response)
{
    session.response.message["message"] = response.text;
    // This is a successful ProviderResponse.  HTTP status for Error is mapped
    // only by the transport sink from platform::ErrorCode.
    session.response.message["statusCode"] = 200;

    if (!response.toolCalls.empty()) {
        Json::Value toolCalls(Json::arrayValue);
        for (const auto& call : response.toolCalls) {
            Json::Value item(Json::objectValue);
            item["id"] = call.id;
            item["name"] = call.name;
            item["arguments"] = call.arguments;
            toolCalls.append(std::move(item));
        }
        session.response.message["tool_calls"] = std::move(toolCalls);
    }

    for (const auto& [key, value] : response.meta) {
        constexpr std::string_view kChaynsPrefix = "chayns.";
        if (key.compare(0, kChaynsPrefix.size(), kChaynsPrefix) != 0) {
            session.response.message["_meta"][key] = value;
            continue;
        }

        const std::string field = key.substr(kChaynsPrefix.size());
        auto& target = session.response.message["_meta"]["chayns"][field];
        if (field == "reasoning_messages") {
            Json::CharReaderBuilder builder;
            Json::Value parsed;
            std::string errors;
            std::istringstream input(value);
            if (Json::parseFromStream(builder, input, &parsed, &errors) && parsed.isArray()) {
                target = std::move(parsed);
            } else {
                target = Json::Value(Json::arrayValue);
            }
        } else if (field == "thread_type_id" || field == "workspace_uac_id") {
            try {
                target = Json::Int64(std::stoll(value));
            } catch (...) {
                target = value;
            }
        } else {
            target = value;
        }
    }
}

std::optional<platform::Error> generation::GenerationPipeline::invokeProvider(
    session_st& session,
    const platform::CancellationToken& cancellation,
    platform::Deadline deadline)
{
    LOG_INFO << "[生成服务] 执行提供者: " << session.request.api;
    if (!providerRegistry_) {
        return platform::Error::internal("Provider registry is unavailable");
    }

    if (const auto chatProvider = providerRegistry_->findChatProvider(session.request.api)) {
        auto request = providerRequestFromSession(session);
        provider::ProviderCallContext context{cancellation, deadline};
        const auto result = chatProvider->generate(request, context);
        if (!result) {
            return result.error();
        }
        applyProviderResponse(session, result.value());
        return std::nullopt;
    }

    return platform::Error::notFound("未找到上游提供者: " + session.request.api);
}
