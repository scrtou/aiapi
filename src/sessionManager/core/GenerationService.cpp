#include "sessionManager/core/GenerationService.h"
#include "sessionManager/core/ClientOutputSanitizer.h"
#include "sessionManager/continuity/ContinuityResolver.h"
#include "sessionManager/continuity/ResponseIndex.h"
#include "sessionManager/tooling/ToolCallBridge.h"
#include "sessionManager/tooling/ToolCallValidator.h"
#include "sessionManager/tooling/XmlTagToolCallCodec.h"
#include "sessionManager/tooling/StrictClientRules.h"
#include "sessionManager/tooling/BridgeHelpers.h"
#include "sessionManager/tooling/ForcedToolCallGenerator.h"
#include "sessionManager/tooling/ToolCallNormalizer.h"
#include "sessionManager/tooling/ToolDefinitionEncoder.h"
#include <apiManager/ApiManager.h>
#include <apipoint/ProviderResult.h>
#include <tools/ZeroWidthEncoder.h>
#include <channelManager/channelManager.h>
#include <metrics/ErrorStatsService.h>
#include <metrics/ErrorEvent.h>
#include <drogon/drogon.h>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_set>

using namespace drogon;
using namespace provider;
using namespace session;
using namespace error;
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
} // 匿名命名空间

std::string GenerationService::computeExecutionKey(const session_st& session) {
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
session_st GenerationService::materializeSession(const GenerationRequest& req) {
    LOG_DEBUG << "[生成服务] 开始将生成请求物化为会话结构";
    
    session_st session;
    
    // 基本参数映射
    session.request.model = req.model;
    session.request.api = req.provider.empty() ? "chaynsapi" : req.provider;
    session.request.systemPrompt = req.systemPrompt;
    session.provider.clientInfo = req.clientInfo;
    session.request.message = req.currentInput;
    session.request.images = req.images;  // 传递图片列表
    session.request.tools = req.tools;           // 传递工具定义
    session.request.toolsRaw = req.tools;       // 保留原始工具定义（用于工具桥接场景下的兜底解析）
    session.request.toolChoice = req.toolChoice; // 传递工具选择策略
    session.request.rawMessage = req.currentInput; // 保留原始输入（工具桥接注入前）
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
        session.addMessageToContext(jsonMsg);
    }
    
    LOG_DEBUG << "[生成服务] 物化完成，模型: " << session.request.model
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
std::optional<AppError> GenerationService::executeGuardedWithSession(
    session_st& session,
    IResponseSink& sink,
    bool stream,
    ConcurrencyPolicy policy
) {
    // 计算执行门控键
    std::string sessionKey = computeExecutionKey(session);
    LOG_DEBUG << "[生成服务] 执行门控, 会话密钥: " << sessionKey
             << ", 策略: " << (policy == ConcurrencyPolicy::RejectConcurrent ? "拒绝并发" : "取消前一个");
    
    // 使用 RAII 执行守卫，确保异常或提前返回时自动释放门控
    ExecutionGuard guard(sessionKey, policy);
    
    if (!guard.isAcquired()) {
        GateResult result = guard.getResult();
        if (result == GateResult::Rejected) {
            LOG_WARN << "[生成服务] 因并发执行被拒绝, 会话密钥: " << sessionKey;
            // 记录 会话_GATE 错误统计
            recordErrorStat(
                session,
                metrics::Domain::SESSION_GATE,
                metrics::EventType::SESSIONGATE_REJECTED_CONFLICT,
                "并发冲突，请求被拒绝"
            );
            return AppError::conflict("当前会话已有进行中的请求，请稍后重试");
        }
        // 其他情况理论上不应该发生
        LOG_ERROR << "[生成服务] 意外的门控结果: " << static_cast<int>(result);
        return AppError::internal("获取执行门控失败");
    }
    
    LOG_DEBUG << "[生成服务] 已获取执行门控, 会话: " << sessionKey;
    
	    try {
	        auto& sessionManager = *chatSession::getInstance();
	        // 每次请求独立字段：仅对当前上游调用有效，进入新请求前必须清空。
	        session.provider.toolBridgeTrigger.clear();
	        
	        // 0. 检查通道是否支持工具调用；若不支持则进入工具桥接模式并注入工具定义
	        bool supportsToolCalls = getChannelSupportsToolCalls(session.request.api);
	        LOG_DEBUG << "[生成服务] 工具能力检查: 是否支持原生工具调用=" << supportsToolCalls
                  << "，tools 是否为空=" << session.request.tools.isNull()
                  << "，tools 是否数组=" << session.request.tools.isArray()
                  << "，tools 数量=" << session.request.tools.size();
	        const Json::Value& toolsForBridge =
	            (!session.request.tools.isNull() && session.request.tools.isArray() && session.request.tools.size() > 0)
	                ? session.request.tools
	                : session.request.toolsRaw;
	        auto normalizeLower = [](std::string s) {
	            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	            return s;
	        };
	        const bool toolChoiceNone = (normalizeLower(session.request.toolChoice) == "none");
	        if (!supportsToolCalls && !toolChoiceNone && !toolsForBridge.isNull() && toolsForBridge.isArray() && toolsForBridge.size() > 0) {
	            LOG_DEBUG << "[生成服务] 通道不支持原生工具调用，已注入工具桥接提示到请求内容";
	            if (session.request.tools.isNull() || !session.request.tools.isArray() || session.request.tools.size() == 0) {
	                session.request.tools = toolsForBridge;
	            }
	            transformRequestForToolBridge(session);
	        }
        
        // 1. Responses 协议：生成 响应Id 并尽早绑定到 响应Index（用于 previous_响应_id 续接）
        if (session.isResponseApi() && session.response.responseId.empty()) {
            session.response.responseId = chatSession::generateResponseId();
        }
        if (session.isResponseApi() && !session.response.responseId.empty()) {
            ResponseIndex::instance().bind(session.response.responseId, session.state.conversationId);
        }

        // 2. 发送 已开始 事件（Responses 使用 响应.； 使用 会话Id，仅用于链路标识）
        generation::Started startEvent;
        startEvent.responseId = session.isResponseApi() ? session.response.responseId : session.state.conversationId;
        startEvent.model = session.request.model;
        sink.onEvent(startEvent);
        
        // 2. 检查取消状态
        if (guard.isCancelled()) {
            LOG_DEBUG << "[生成服务] 调用提供者前请求被取消";
            // 记录 会话_GATE 取消统计
            recordWarnStat(
                session,
                metrics::Domain::SESSION_GATE,
                metrics::EventType::SESSIONGATE_CANCELLED,
                "调用上游前请求已被取消"
            );
            emitError(generation::ErrorCode::Cancelled, "请求已取消", sink);
            sink.onClose();
            return AppError::cancelled("请求已取消");
        }
        
        // 3. 调用上游接口
        if (!executeProvider(session)) {
            // 记录 UPSTREAM 错误统计（上游 错误）
            int httpStatus = session.response.message.get("statusCode", 0).asInt();
            std::string errorMsg = safeJsonAsString(session.response.message.get("error", "上游服务错误"), "上游服务错误");
            recordErrorStat(
                session,
                metrics::Domain::UPSTREAM,
                metrics::EventType::UPSTREAM_HTTP_ERROR,
                errorMsg,
                httpStatus
            );
            emitError(
                generation::ErrorCode::ProviderError,
                errorMsg,
                sink
            );
            sink.onClose();
            // 记录请求完成（失败分支）
            recordRequestCompletedStat(session, httpStatus);
            return std::nullopt;  // 上游 错误已通过 发送
        }
        
        // 4. 检查取消状态
        if (guard.isCancelled()) {
            LOG_DEBUG << "[生成服务] 调用提供者后请求被取消";
            // 记录 会话_GATE 取消统计
            recordWarnStat(
                session,
                metrics::Domain::SESSION_GATE,
                metrics::EventType::SESSIONGATE_CANCELLED,
                "请求已取消 after provider call"
            );
            emitError(generation::ErrorCode::Cancelled, "请求已取消", sink);
            sink.onClose();
            return AppError::cancelled("请求已取消");
        }
        
        // 5. 预生成下一轮 会话Id（用于在响应文本中嵌入，支持客户端续聊）
        // 必须在 emitResultEvents 之前调用，这样嵌入的是新 ID
        // ZeroWidth 和 Hash 模式都需要每轮生成新的 会话Id
        sessionManager.prepareNextSessionId(session);
        
        // 6. 发送结果事件（内部会使用 会话..next会话Id 进行会话标识嵌入）
        emitResultEvents(session, sink);
        
        // 7. 更新会话上下文并执行会话转移（延迟提交）
        // cover会话响应() 内部会检测 next会话Id，如果存在则调用 commit会话Transfer()
        // 完成：更新 messageContext → 转移会话到新 会话Id → 更新 会话_map
        if (session.isResponseApi() && !session.response.responseId.empty()) {
            session.response.lastResponseId = session.response.responseId;
        }
        sessionManager.coverSessionresponse(session);
        if (session.isResponseApi() && !session.response.responseId.empty()) {
            // 会话转移后 conversationId 已更新为 next会话Id，重新绑定 响应Id
            ResponseIndex::instance().bind(session.response.responseId, session.state.conversationId);
        }
        
        // 8. 执行上游响应后处理
        auto api = ApiManager::getInstance().getApiByApiName(session.request.api);
        if (api) {
            api->afterResponseProcess(session);
        }
        
        // 9. 记录请求完成（成功分支）
        recordRequestCompletedStat(session, 200);
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[生成服务] 执行门控会话异常: " << e.what();
        // 记录内部异常统计
        recordErrorStat(
            session,
            metrics::Domain::INTERNAL,
            metrics::EventType::INTERNAL_EXCEPTION,
            e.what(),
            500
        );
        emitError(generation::ErrorCode::Internal, e.what(), sink);
    } catch (...) {
        LOG_ERROR << "[生成服务] 执行门控会话未知异常";
        // 记录内部未知异常统计
        recordErrorStat(
            session,
            metrics::Domain::INTERNAL,
            metrics::EventType::INTERNAL_UNKNOWN,
            "发生未知错误",
            500
        );
        emitError(generation::ErrorCode::Internal, "发生未知错误", sink);
    }
    
    sink.onClose();
    return std::nullopt;  // ExecutionGuard 在析构时自动释放门控，无需手动清理
}

// ========== 新主入口实现（ 统一调用） ==========

std::optional<AppError> GenerationService::runGuarded(
    const GenerationRequest& req,
    IResponseSink& sink,
    ConcurrencyPolicy policy
) {
    LOG_INFO << "[生成服务] 进入 runGuarded 主入口，协议："
             << (req.isResponseApi() ? "Responses" : "ChatCompletions")
             << ", 流式: " << req.stream;
    
    // 1. 物化请求：Generation请求 → 会话_st
    session_st session = materializeSession(req);
    
    // 2. 解析 会话Id 并 getOr创建（必须先于门控）
    auto& sessionManager = *chatSession::getInstance();

    ContinuityResolver resolver;
    const ContinuityDecision decision = resolver.resolve(req);

    LOG_DEBUG << "[生成服务] 连续性决策"
             << " 来源=" << static_cast<int>(decision.source)
             << " 模式=" << (decision.mode == SessionTrackingMode::ZeroWidth ? "ZeroWidth" : "Hash")
             << " 会话ID=" << decision.sessionId
             << (decision.debug.empty() ? "" : (" 调试信息=" + decision.debug));

    sessionManager.getOrCreateSession(decision.sessionId, session);

    LOG_INFO << "[生成服务] 会话 " << (session.state.isContinuation ? "续接" : "新建")
             << ", 会话ID: " << session.state.conversationId
             << ", 协议类型: " << (session.isResponseApi() ? "Responses" : "ChatCompletions");
    
    // 3. 调用共享执行函数 executeGuardedWith会话()
    return executeGuardedWithSession(session, sink, req.stream, policy);
}

bool GenerationService::executeProvider(session_st& session) {
    LOG_DEBUG << "[生成服务] 执行提供者: " << session.request.api;
    
    auto api = ApiManager::getInstance().getApiByApiName(session.request.api);
    if (!api) {
        LOG_ERROR << "[生成服务] 未找到提供者: " << session.request.api;
        session.response.message["error"] = "未找到上游提供者: " + session.request.api;
        return false;
    }
    
    // 使用 () 接口获取结构化结果
    ProviderResult result = api->generate(session);

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
    
    // 将结果写回 会话.响应. 以保持旧链路兼容
    session.response.message["message"] = result.text;
    session.response.message["statusCode"] = result.statusCode;
    
    if (!result.isSuccess()) {
        LOG_ERROR << "[生成服务] 上游返回错误，状态码: " << result.statusCode
                  << ", 消息: " << result.error.message;
        session.response.message["error"] = result.error.message;
        return false;
    }
    
    return true;
}

/**
 * @brief 发送生成结果事件到客户端
 *
 * 这是响应处理的核心函数，负责将上游模型的原始输出转换为客户端可理解的事件流。
 *
 * 【处理流程概览】
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  1. 文本清洗（sanitizeOutput）                                           │
 * │     ↓                                                                   │
 * │  2. 工具调用解析（Bridge模式：extractXmlInputForToolCalls + parseXml） │
 * │     ↓                                                                   │
 * │  3. 参数形状规范化（normalizeToolCallArguments）                          │
 * │     ↓                                                                   │
 * │  4. Schema校验与无效调用过滤（ToolCallValidator，避免缺少 nativeArgs） │
 * │     ↓                                                                   │
 * │  5. 自愈逻辑（selfHealReadFile，仅 Roo/Kilo）                           │
 * │     ↓                                                                   │
 * │  6. 严格客户端规则（applyStrictClientRules，仅 Roo/Kilo）               │
 * │     ↓                                                                   │
 * │  7. 零宽字符会话ID嵌入                                                   │
 * │     ↓                                                                   │
 * │  8. 发送事件：ToolCallDone → OutputTextDone → Completed                 │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * @param session 会话状态，包含请求/响应数据、工具定义等
 * @param sink 事件接收器，负责将事件序列化并发送给客户端
 */
