#include "GenerationService.h"
#include "ClientOutputSanitizer.h"
#include "ToolCallBridge.h"
#include "ToolCallValidator.h"
#include "XmlTagToolCallCodec.h"
#include <apiManager/ApiManager.h>
#include <apipoint/ProviderResult.h>
#include <tools/ZeroWidthEncoder.h>
#include <dbManager/channel/channelDbManager.h>
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

// ========== 共享 helper 实现 ==========

namespace {

// ========== 通用辅助函数 ==========

std::string trimWhitespace(std::string s) {
    const auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::string stripMarkdownCodeFence(std::string s) {
    s = trimWhitespace(std::move(s));
    if (s.rfind("```", 0) != 0) return s;
    size_t firstNl = s.find('\n');
    if (firstNl == std::string::npos) return s;
    size_t lastFence = s.rfind("```");
    if (lastFence == std::string::npos || lastFence <= firstNl) return s;
    return trimWhitespace(s.substr(firstNl + 1, lastFence - (firstNl + 1)));
}


// Normalize whitespace that may break the XML tool-call bridge parser.
// - NBSP (U+00A0) encoded as UTF-8: 0xC2 0xA0
// - IDEOGRAPHIC SPACE (U+3000) encoded as UTF-8: 0xE3 0x80 0x80
static void replaceAllBytes(std::string& s, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    std::string normalizeBridgeXml(std::string s) {
        // Normalize line endings
        replaceAllBytes(s, "\r\n", "\n");
        replaceAllBytes(s, "\r", "\n");
    
        // Normalize non-ASCII spaces that some UIs/models output (or templates accidentally contain)
        replaceAllBytes(s, "\xC2\xA0", " ");       // NBSP
        replaceAllBytes(s, "\xE3\x80\x80", " ");   // U+3000
    
        return s;
}
    

// (removed) JSON tool-call parsing helper (bridge output is XML-only now)

std::string toLowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 安全地将 Json::Value 转换为字符串，处理非字符串类型
std::string safeJsonAsString(const Json::Value& val, const std::string& defaultVal = "") {
    if (val.isString()) {
        return val.asString();
    }
    if (val.isNull()) {
        return defaultVal;
    }
    // 对于其他类型（对象、数组、数字等），序列化为 JSON 字符串
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, val);
}

std::string_view ltrimView(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    return std::string_view(s).substr(i);
}

bool startsWithStr(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

size_t findFunctionCallsPos(std::string_view s) {
    size_t p1 = s.find("<function_calls");
    size_t p2 = s.find("<antml:function_calls");
    if (p1 == std::string_view::npos) return p2;
    if (p2 == std::string_view::npos) return p1;
    return (p1 < p2) ? p1 : p2;
}
/*
std::string extractXmlInputForToolCalls(const session_st& session, const std::string& rawText) {
    // Avoid false positives:
    // Some model outputs include <function_calls> blocks as examples inside normal text.
    // Prefer parsing the Toolify-style trigger marker (<Function_..._Start/>) when present.
    // Otherwise, only parse when the response *starts* with a tool marker.
    std::string xmlCandidate = stripMarkdownCodeFence(rawText);
    const std::string_view trimmed = ltrimView(xmlCandidate);

    std::string xmlInput;

    if (!session.tool_bridge_trigger.empty()) {
        // Only parse tool calls belonging to this request when the trigger matches.
        // However, upstream UIs/models occasionally echo an older trigger marker. If the
        // response starts with a tool marker, still parse it to avoid dropping valid calls.
        size_t triggerPos = xmlCandidate.rfind(session.tool_bridge_trigger);
        if (triggerPos != std::string::npos) {
            xmlInput = xmlCandidate.substr(triggerPos);

            // Strip the Toolify trigger marker (<Function_..._Start/>) so the XML codec
            // doesn't emit it as normal text before <function_calls>.
            const std::string_view t2 = ltrimView(xmlInput);
            if (startsWithStr(t2, "<Function_")) {
                size_t fcPos = findFunctionCallsPos(t2);
                if (fcPos != std::string_view::npos && fcPos <= 512) {
                    xmlInput = std::string(t2.substr(fcPos));
                }
            }
        } else {
            const bool startsLikeToolCall =
                startsWithStr(trimmed, "<function_calls") ||
                startsWithStr(trimmed, "<antml:function_calls") ||
                startsWithStr(trimmed, "<Function_");
            if (startsLikeToolCall) {
                if (startsWithStr(trimmed, "<Function_")) {
                    // Require a real <function_calls> tag very early to reduce false positives.
                    size_t fcPos = findFunctionCallsPos(trimmed);
                    if (fcPos != std::string_view::npos && fcPos <= 512) {
                        // Strip the trigger marker line, keep only the <function_calls> XML.
                        xmlInput = std::string(trimmed.substr(fcPos));
                        LOG_WARN << "[GenerationService] tool_bridge_trigger 不匹配，已从响应开头解析 tool call";
                    }
                } else {
                    xmlInput = std::string(trimmed);
                    LOG_WARN << "[GenerationService] tool_bridge_trigger 缺失，已从响应开头解析 tool call";
                }
            }
        }
    } else {
        // Legacy fixed trigger fallback (backwards compatible).
        size_t triggerPos = xmlCandidate.rfind("<Function_o2gx_Start/>");
        if (triggerPos != std::string::npos) {
            xmlInput = xmlCandidate.substr(triggerPos);

            // Same as above: avoid leaking the trigger marker as normal text.
            const std::string_view t2 = ltrimView(xmlInput);
            if (startsWithStr(t2, "<Function_")) {
                size_t fcPos = findFunctionCallsPos(t2);
                if (fcPos != std::string_view::npos && fcPos <= 512) {
                    xmlInput = std::string(t2.substr(fcPos));
                }
            }
        } else {
            const bool looksLikeToolCall =
                startsWithStr(trimmed, "<function_calls") ||
                startsWithStr(trimmed, "<antml:function_calls") ||
                startsWithStr(trimmed, "<Function_");
            if (looksLikeToolCall) {
                if (startsWithStr(trimmed, "<Function_")) {
                    // Strip the trigger marker and parse only the XML.
                    size_t fcPos = findFunctionCallsPos(trimmed);
                    if (fcPos != std::string_view::npos && fcPos <= 512) {
                        xmlInput = std::string(trimmed.substr(fcPos));
                    }
                } else {
                    xmlInput = std::string(trimmed);
                }
            }
        }
    }

    return xmlInput;
}
*/
// GenerationService.cpp 中的 helper 函数

std::string extractXmlInputForToolCalls(const session_st& session, const std::string& rawText) {
    std::string xmlCandidate = stripMarkdownCodeFence(rawText);
    const std::string_view trimmed = ltrimView(xmlCandidate);

    // 1. 如果有随机 Trigger，优先使用它定位
    if (!session.tool_bridge_trigger.empty()) {
        size_t triggerPos = trimmed.find(session.tool_bridge_trigger);
        if (triggerPos != std::string::npos) {
            // [关键修改] 直接返回包含 Trigger 的子串，不要在后面把 Trigger 切掉！
            // 这样 Codec 才能读到 "<Function_.../>" 并进行匹配
            return std::string(trimmed.substr(triggerPos));
        }
    }

    // 2. 兜底逻辑：如果找不到 Trigger，尝试直接找 <function_calls>
    // 这种情况下，返回的字符串不含 Sentinel，Codec 的 setSentinel 应该传入空，或者 Codec 发现不匹配时回退
    size_t tagPos = trimmed.find("<function_calls"); // 简略匹配
    if (tagPos == std::string::npos) tagPos = trimmed.find("<antml:function_calls");
    
    if (tagPos != std::string::npos) {
        return std::string(trimmed.substr(tagPos));
    }

    return "";
}
std::string generateFallbackToolCallId() {
    // Keep the same "call_xxx" style as common tool call ids
    std::ostringstream oss;
    oss << "call_" << std::hex << std::setfill('0');

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    for (int i = 0; i < 12; ++i) {
        oss << std::setw(2) << dis(gen);
    }

    return oss.str();
}

std::string generateRandomTriggerSignal() {
    // Toolify-style random trigger, e.g. <Function_AB1c_Start/>
    static const std::string kChars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, static_cast<int>(kChars.size() - 1));

    std::string randomStr;
    randomStr.reserve(4);
    for (int i = 0; i < 4; ++i) {
        randomStr.push_back(kChars[dis(gen)]);
    }

    return "<Function_" + randomStr + "_Start/>";
}

// ========== 错误统计辅助函数 ==========

/**
 * @brief 从 session 中提取客户端类型
 */
std::string getClientTypeFromSession(const session_st& session) {
    return safeJsonAsString(session.client_info.get("client_type", ""), "");
}

/**
 * @brief 获取 API 类型字符串
 */
std::string getApiKindFromSession(const session_st& session) {
    return session.is_response_api ? "responses" : "chat_completions";
}

/**
 * @brief 记录错误事件到统计服务
 */
void recordErrorStat(
    const session_st& session,
    metrics::Domain domain,
    const std::string& type,
    const std::string& message,
    int httpStatus = 0,
    const Json::Value& detail = Json::Value(),
    const std::string& rawSnippet = "",
    const std::string& toolName = ""
) {
    metrics::ErrorStatsService::getInstance().recordError(
        domain,
        type,
        message,
        session.request_id,
        session.selectapi,
        session.selectmodel,
        getClientTypeFromSession(session),
        getApiKindFromSession(session),
        false,  // stream - 暂时设为 false
        httpStatus,
        detail,
        rawSnippet,
        toolName
    );
}

/**
 * @brief 记录警告事件到统计服务
 */
void recordWarnStat(
    const session_st& session,
    metrics::Domain domain,
    const std::string& type,
    const std::string& message,
    const Json::Value& detail = Json::Value(),
    const std::string& rawSnippet = "",
    const std::string& toolName = ""
) {
    metrics::ErrorStatsService::getInstance().recordWarn(
        domain,
        type,
        message,
        session.request_id,
        session.selectapi,
        session.selectmodel,
        getClientTypeFromSession(session),
        getApiKindFromSession(session),
        false,  // stream
        0,      // httpStatus
        detail,
        rawSnippet,
        toolName
    );
}

/**
 * @brief 记录请求完成事件
 */
void recordRequestCompletedStat(const session_st& session, int httpStatus) {
    metrics::RequestCompletedData data;
    data.provider = session.selectapi;
    data.model = session.selectmodel;
    data.clientType = getClientTypeFromSession(session);
    data.apiKind = getApiKindFromSession(session);
    data.stream = false;  // 暂时设为 false
    data.httpStatus = httpStatus;
    data.ts = std::chrono::system_clock::now();
    
    metrics::ErrorStatsService::getInstance().recordRequestCompleted(data);
}

} // namespace

std::string GenerationService::computeExecutionKey(const session_st& session) {
    // Response API: 优先使用 response_id
    // Chat API: 使用 curConversationId
    if (!session.response_id.empty()) {
        return session.response_id;
    }
    return session.curConversationId;
}

session_st GenerationService::materializeSession(const GenerationRequest& req) {
    LOG_INFO << "[生成服务] 物化 GenerationRequest -> session_st";
    
    session_st session;
    
    // 基本参数映射
    session.selectmodel = req.model;
    session.selectapi = req.provider.empty() ? "chaynsapi" : req.provider;
    session.systemprompt = req.systemPrompt;
    session.client_info = req.clientInfo;
    session.requestmessage = req.currentInput;
    session.requestImages = req.images;  // 传递图片列表
    session.tools = req.tools;           // 传递工具定义
    session.tools_raw = req.tools;       // 保留原始工具定义（用于 tool bridge 兜底）
    session.toolChoice = req.toolChoice; // 传递工具选择策略
    session.requestmessage_raw = req.currentInput; // 保留原始输入（tool bridge 注入前）
    session.last_active_time = time(nullptr);
    session.created_time = time(nullptr);
    
    // 协议相关
    session.is_response_api = req.isResponseApi();

    // Responses API: allow controller to pre-assign a response_id so all layers use the same id.
    if (session.is_response_api && !req.responseId.empty()) {
        session.response_id = req.responseId;
    }
    
    // 会话 key（如果已在 RequestAdapters 中从零宽字符提取）
    if (!req.sessionKey.empty()) {
        session.curConversationId = req.sessionKey;
        session.preConversationId = req.sessionKey;
    }
    
    // previous key（用于 Responses API 续聊）
    if (!req.previousKey.empty()) {
        session.has_previous_response_id = true;
    }
    
    // 转换消息上下文（从强类型 Message 转换为 Json::Value）
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
    
    LOG_INFO << "[生成服务] 物化完成, model: " << session.selectmodel
             << ", is_response_api: " << session.is_response_api
             << ", message_context size: " << session.message_context.size();
    
    return session;
}

std::optional<AppError> GenerationService::executeGuardedWithSession(
    session_st& session,
    IResponseSink& sink,
    bool stream,
    ConcurrencyPolicy policy
) {
    // 计算执行 key
    std::string sessionKey = computeExecutionKey(session);
    
    if (sessionKey.empty()) {
        LOG_WARN << "[生成服务] 无会话密钥, 不使用门控运行";
        runWithSession(session, sink, stream);
        return std::nullopt;
    }
    
    LOG_INFO << "[生成服务] 执行门控, 会话密钥: " << sessionKey
             << ", 策略: " << (policy == ConcurrencyPolicy::RejectConcurrent ? "拒绝并发" : "取消前一个");
    
    // 使用 RAII 执行守卫
    ExecutionGuard guard(sessionKey, policy);
    
    if (!guard.isAcquired()) {
        GateResult result = guard.getResult();
        if (result == GateResult::Rejected) {
            LOG_WARN << "[生成服务] 因并发执行被拒绝, 会话密钥: " << sessionKey;
            // 记录 SESSION_GATE 错误统计
            recordErrorStat(
                session,
                metrics::Domain::SESSION_GATE,
                metrics::EventType::SESSIONGATE_REJECTED_CONFLICT,
                "Request rejected due to concurrent execution",
                409  // HTTP 409 Conflict
            );
            return AppError::conflict("Another request is already in progress for this session");
        }
        // 其他情况理论上不应该发生
        LOG_ERROR << "[生成服务] 意外的门控结果: " << static_cast<int>(result);
        return AppError::internal("Failed to acquire execution gate");
    }
    
    LOG_INFO << "[生成服务] 已获取执行门控, 会话: " << sessionKey;
    
	    try {
	        auto& sessionManager = *chatSession::getInstance();
	        // Per-request field: only valid for the current upstream call.
	        session.tool_bridge_trigger.clear();
	        
	        // 0. 检查通道是否支持 tool calls，如果不支持则注入工具定义到 systemprompt 和 requestmessage
	        bool supportsToolCalls = getChannelSupportsToolCalls(session.selectapi);
	        LOG_INFO<<" supportsToolCalls"<<supportsToolCalls<<" session.tools.isNull()"<<session.tools.isNull()<<"session.tools.isArray()"<<session.tools.isArray()<<"session.tools.size()"<<session.tools.size();
	        const Json::Value& toolsForBridge =
	            (!session.tools.isNull() && session.tools.isArray() && session.tools.size() > 0)
	                ? session.tools
	                : session.tools_raw;
	        auto normalizeLower = [](std::string s) {
	            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	            return s;
	        };
	        const bool toolChoiceNone = (normalizeLower(session.toolChoice) == "none");
	        if (!supportsToolCalls && !toolChoiceNone && !toolsForBridge.isNull() && toolsForBridge.isArray() && toolsForBridge.size() > 0) {
	            LOG_INFO << "[生成服务] 通道不支持 tool calls，注入工具定义到 systemprompt 和 requestmessage";
	            if (session.tools.isNull() || !session.tools.isArray() || session.tools.size() == 0) {
	                session.tools = toolsForBridge;
	            }
	            transformRequestForToolBridge(session);
	        }
        
        // 1. 发送 Started 事件
        generation::Started startEvent;
        startEvent.responseId = session.curConversationId;
        startEvent.model = session.selectmodel;
        sink.onEvent(startEvent);
        
        // 2. 检查取消状态
        if (guard.isCancelled()) {
            LOG_INFO << "[生成服务] 调用提供者前请求被取消";
            // 记录 SESSION_GATE 取消统计
            recordWarnStat(
                session,
                metrics::Domain::SESSION_GATE,
                metrics::EventType::SESSIONGATE_CANCELLED,
                "Request cancelled before provider call"
            );
            emitError(generation::ErrorCode::Cancelled, "Request cancelled", sink);
            sink.onClose();
            return AppError::cancelled("Request was cancelled");
        }
        
        // 3. 调用 Provider
        if (!executeProvider(session)) {
            // 记录 UPSTREAM 错误统计（Provider 错误）
            int httpStatus = session.responsemessage.get("statusCode", 0).asInt();
            std::string errorMsg = safeJsonAsString(session.responsemessage.get("error", "Provider error"), "Provider error");
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
            // 记录请求完成（失败）
            recordRequestCompletedStat(session, httpStatus);
            return std::nullopt;  // Provider 错误已通过 sink 发送
        }
        
        // 4. 检查取消状态
        if (guard.isCancelled()) {
            LOG_INFO << "[生成服务] 调用提供者后请求被取消";
            // 记录 SESSION_GATE 取消统计
            recordWarnStat(
                session,
                metrics::Domain::SESSION_GATE,
                metrics::EventType::SESSIONGATE_CANCELLED,
                "Request cancelled after provider call"
            );
            emitError(generation::ErrorCode::Cancelled, "Request cancelled", sink);
            sink.onClose();
            return AppError::cancelled("Request was cancelled");
        }
        
        // 5. 发送结果事件
        emitResultEvents(session, sink);
        
        // 6. 更新会话上下文（根据 API 类型选择不同方法）
        if (session.is_response_api) {
            sessionManager.updateResponseSession(session);
        } else {
            sessionManager.coverSessionresponse(session);
        }
        
        // 7. 调用 Provider 后处理
        auto api = ApiManager::getInstance().getApiByApiName(session.selectapi);
        if (api) {
            api->afterResponseProcess(session);
        }
        
        // 8. 记录请求完成（成功）
        recordRequestCompletedStat(session, 200);
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[生成服务] 执行门控会话异常: " << e.what();
        // 记录 INTERNAL 异常统计
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
        // 记录 INTERNAL 未知异常统计
        recordErrorStat(
            session,
            metrics::Domain::INTERNAL,
            metrics::EventType::INTERNAL_UNKNOWN,
            "Unknown error occurred",
            500
        );
        emitError(generation::ErrorCode::Internal, "Unknown error occurred", sink);
    }
    
    sink.onClose();
    return std::nullopt;  // ExecutionGuard 析构时自动释放
}

// ========== 新主入口实现 ==========

std::optional<AppError> GenerationService::runGuarded(
    const GenerationRequest& req,
    IResponseSink& sink,
    ConcurrencyPolicy policy
) {
    LOG_INFO << "[生成服务] runGuarded 新主入口, 协议: "
             << (req.isResponseApi() ? "Responses" : "ChatCompletions")
             << ", 流式: " << req.stream;
    
    // 1. materialize: GenerationRequest → session_st
    session_st session = materializeSession(req);
    
    // 2. create/update session（必须发生在门控前）
    auto& sessionManager = *chatSession::getInstance();
    
    if (req.isResponseApi()) {
        // Response API 流程
        // 设置 previous_response_id 标记（如果有）
        if (!req.previousKey.empty()) {
            session.curConversationId = req.previousKey;
            session.has_previous_response_id = true;
        }
        
        // 使用统一包装方法创建/更新 Response 会话
        // previous_response_id 的上下文继承逻辑已集成在 createOrUpdateResponseSession 中
        session = sessionManager.createOrUpdateResponseSession(session);
        
        // 创建 Response 会话（获取稳定的 response_id 用于门控）
        // NOTE: Provider 线程上下文通常按 "会话ID" 维度存储。
        // Responses API 每一轮会生成新的 response_id；若不把 provider 的上下文从上一轮 key
        // 转移到新的 response_id，则 provider 会不断创建新线程，表现为“找不到上一轮会话”。
        const std::string prevProviderKey = session.curConversationId;
        std::string responseId = sessionManager.createResponseSession(session);
        LOG_INFO << "[生成服务] 创建 Response 会话, response_id: " << responseId;

        // Transfer provider thread context (best-effort)
        if (!session.selectapi.empty() && !prevProviderKey.empty() && prevProviderKey != responseId) {
            auto api = ApiManager::getInstance().getApiByApiName(session.selectapi);
            if (api) {
                api->transferThreadContext(prevProviderKey, responseId);
            }
        }
    } else {
        // Chat API 流程 - 使用统一包装方法
        session = sessionManager.createOrUpdateChatSession(session);
        LOG_INFO << "[生成服务] Chat 会话已创建/更新, curConversationId: " << session.curConversationId;
    }
    
    // 3. 调用共享 helper executeGuardedWithSession()
    return executeGuardedWithSession(session, sink, req.stream, policy);
}

// ========== 旧入口实现（变薄，调用共享 helper）==========

void GenerationService::run(const GenerationRequest& req, IResponseSink& sink) {
    LOG_INFO << "[生成服务] 开始生成, 协议: " 
             << (req.isResponseApi() ? "Responses" : "ChatCompletions")
             << ", 流式: " << req.stream;
    
    try {
        // 构建或获取 session
        session_st session;
        session.selectmodel = req.model;
        session.selectapi = req.provider.empty() ? "chaynsapi" : req.provider;
        session.systemprompt = req.systemPrompt;
        session.client_info = req.clientInfo;
        session.requestmessage = req.currentInput;
        session.last_active_time = time(nullptr);
        
        // 根据协议选择不同的处理流程
        if (req.isResponseApi()) {
            runResponseFlow(req, sink, session);
        } else {
            runChatFlow(req, sink, session);
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[生成服务] 异常: " << e.what();
        emitError(generation::ErrorCode::Internal, e.what(), sink);
    } catch (...) {
        LOG_ERROR << "[生成服务] 未知异常";
        emitError(generation::ErrorCode::Internal, "Unknown error occurred", sink);
    }
    
    sink.onClose();
}

void GenerationService::runWithSession(session_st& session, IResponseSink& sink, bool stream) {
    LOG_INFO << "[生成服务] 运行会话, 流式: " << stream;
    
    try {
        auto& sessionManager = *chatSession::getInstance();
        
        // 1. 发送 Started 事件
        generation::Started startEvent;
        startEvent.responseId = session.curConversationId;
        startEvent.model = session.selectmodel;
        sink.onEvent(startEvent);
        
        // 2. 调用 Provider
        if (!executeProvider(session)) {
            emitError(
                generation::ErrorCode::ProviderError,
                safeJsonAsString(session.responsemessage.get("error", "Provider error"), "Provider error"),
                sink
            );
            sink.onClose();
            return;
        }
        
        // 3. 发送结果事件
        emitResultEvents(session, sink);
        
        // 4. 更新会话上下文
        sessionManager.coverSessionresponse(session);
        
        // 5. 调用 Provider 后处理
        auto api = ApiManager::getInstance().getApiByApiName(session.selectapi);
        if (api) {
            api->afterResponseProcess(session);
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[生成服务] 运行会话异常: " << e.what();
        emitError(generation::ErrorCode::Internal, e.what(), sink);
    } catch (...) {
        LOG_ERROR << "[生成服务] 运行会话未知异常";
        emitError(generation::ErrorCode::Internal, "Unknown error occurred", sink);
    }
    
    sink.onClose();
}

std::optional<AppError> GenerationService::runWithSessionGuarded(
    session_st& session,
    IResponseSink& sink,
    bool stream,
    ConcurrencyPolicy policy
) {
    LOG_INFO << "[生成服务] runWithSessionGuarded (旧入口-变薄), 流式: " << stream
             << ", 策略: " << (policy == ConcurrencyPolicy::RejectConcurrent ? "拒绝并发" : "取消前一个");
    
    // 直接调用共享 helper，复用同一套门控/执行包装逻辑
    return executeGuardedWithSession(session, sink, stream, policy);
}

void GenerationService::runChatFlow(
    const GenerationRequest& req,
    IResponseSink& sink,
    session_st& session
) {
    LOG_INFO << "[生成服务] 运行聊天流程";
    
    // 1. 创建或更新会话 - 使用统一包装方法
    auto& sessionManager = *chatSession::getInstance();
    session = sessionManager.createOrUpdateChatSession(session);
    
    // 2. 发送 Started 事件
    generation::Started startEvent;
    startEvent.responseId = session.curConversationId;
    startEvent.model = session.selectmodel;
    sink.onEvent(startEvent);
    
    // 3. 调用 Provider
    if (!executeProvider(session)) {
        emitError(
            generation::ErrorCode::ProviderError,
            safeJsonAsString(session.responsemessage.get("error", "Provider error"), "Provider error"),
            sink
        );
        return;
    }
    
    // 4. 发送结果事件
    emitResultEvents(session, sink);
    
    // 5. 更新会话上下文
    sessionManager.coverSessionresponse(session);
}

void GenerationService::runResponseFlow(
    const GenerationRequest& req, 
    IResponseSink& sink, 
    session_st& session
) {
    LOG_INFO << "[生成服务] 运行响应流程";
    
    auto& sessionManager = *chatSession::getInstance();
    
    // 1. 处理 previous_response_id (如果有)
    if (!req.previousKey.empty()) {
        session_st prevSession;
        if (sessionManager.getResponseSession(req.previousKey, prevSession)) {
            session.message_context = prevSession.message_context;
            session.preConversationId = req.previousKey;
        }
    }
    
    // 2. 创建 Response 会话
    session.is_response_api = true;
    std::string responseId = sessionManager.createResponseSession(session);
    
    // 3. 发送 Started 事件
    generation::Started startEvent;
    startEvent.responseId = responseId;
    startEvent.model = session.selectmodel;
    sink.onEvent(startEvent);
    
    // 4. 调用 Provider
    if (!executeProvider(session)) {
        emitError(
            generation::ErrorCode::ProviderError,
            safeJsonAsString(session.responsemessage.get("error", "Provider error"), "Provider error"),
            sink
        );
        return;
    }
    
    // 5. 发送结果事件
    emitResultEvents(session, sink);
    
    // 6. 更新 Response 会话
    sessionManager.updateResponseSession(session);
}

bool GenerationService::executeProvider(session_st& session) {
    LOG_INFO << "[生成服务] 执行提供者: " << session.selectapi;
    
    auto api = ApiManager::getInstance().getApiByApiName(session.selectapi);
    if (!api) {
        LOG_ERROR << "[生成服务] 未找到提供者: " << session.selectapi;
        session.responsemessage["error"] = "Provider not found: " + session.selectapi;
        return false;
    }
    
    // 使用新的 generate() 接口获取结构化结果
    ProviderResult result = api->generate(session);
    
    // 将结果写回 session.responsemessage 以保持向后兼容
    session.responsemessage["message"] = result.text;
    session.responsemessage["statusCode"] = result.statusCode;
    
    if (!result.isSuccess()) {
        LOG_ERROR << "[生成服务] 提供者返回错误, 状态码: " << result.statusCode
                  << ", 消息: " << result.error.message;
        session.responsemessage["error"] = result.error.message;
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
 * │  1. 文本清洗 (sanitizeOutput)                                           │
 * │     ↓                                                                   │
 * │  2. Tool Call 解析 (Bridge模式: extractXmlInputForToolCalls + parseXml) │
 * │     ↓                                                                   │
 * │  3. 参数形状规范化 (normalizeToolCallArguments)                          │
 * │     ↓                                                                   │
 * │  4. Schema校验 + 过滤无效调用 (ToolCallValidator)  ← 防止 missing nativeArgs │
 * │     ↓                                                                   │
 * │  5. 自愈逻辑 (selfHealReadFile) - 仅 Roo/Kilo                           │
 * │     ↓                                                                   │
 * │  6. 严格客户端规则 (applyStrictClientRules) - 仅 Roo/Kilo               │
 * │     ↓                                                                   │
 * │  7. 零宽字符会话ID嵌入                                                   │
 * │     ↓                                                                   │
 * │  8. 发送事件: ToolCallDone → OutputTextDone → Completed                 │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * @param session 会话状态，包含请求/响应数据、工具定义等
 * @param sink 事件接收器，负责将事件序列化并发送给客户端
 */
void GenerationService::emitResultEvents(const session_st& session, IResponseSink& sink) {
    // ==================== 步骤 0: 初始化 ====================
    // 从 session.responsemessage 中提取上游模型返回的原始文本
    std::string text = safeJsonAsString(session.responsemessage.get("message", ""), "");
    
    // 获取客户端类型，用于后续的客户端特定处理
    const std::string clientType = safeJsonAsString(session.client_info.get("client_type", ""), "");
    
    // 判断是否为"严格工具客户端"（Roo/Kilo）
    // 这类客户端要求：每次响应必须且只能包含 1 个 tool call
    const bool strictToolClient = (clientType == "Kilo-Code" || clientType == "RooCode");

    // ==================== 步骤 1: 文本清洗 ====================
    // 调用 sanitizeOutput() 进行基础文本清洗：
    // - 修复常见的 XML/HTML 标签错误
    // - 移除不可见控制字符
    // - 针对特定客户端的输出格式调整
    text = sanitizeOutput(session.client_info, text);

    // ==================== 步骤 2: Tool Call 解析 ====================
    // 检查当前通道是否原生支持 tool calls
    // 如果支持，上游会直接返回结构化的 tool_calls，无需从文本中解析
    const bool supportsToolCalls = getChannelSupportsToolCalls(session.selectapi);
    
    // 检查 tool_choice 是否为 "none"（禁用工具调用）
    const bool toolChoiceNone = (toLowerStr(session.toolChoice) == "none");

    // 用于存储解析结果
    std::string textContent;                              // 普通文本内容
    std::vector<generation::ToolCallDone> toolCalls;      // 解析出的工具调用列表

    // 【解析策略】
    // - 通道原生支持 tool calls → 不从文本解析，直接使用原始文本
    // - tool_choice=none → 禁用工具，全部当作普通文本
    // - 其他情况（Bridge 模式）→ 从文本中解析 XML 格式的工具调用
    if (supportsToolCalls || toolChoiceNone) {
        // 通道支持原生 tool calls 或禁用工具：直接使用原始文本
        textContent = text;
    } else {
        // ========== Bridge 模式：从文本中解析 XML 格式的工具调用 ==========
        //
        // 【为什么需要 Bridge 模式？】
        // 某些上游通道（如部分 OpenAI 兼容 API）不支持原生 tool_calls，
        // 我们通过在 prompt 中注入工具定义，让模型以 XML 格式输出工具调用，
        // 然后在这里解析 XML 并转换为标准的 tool_calls 结构。
        
        // 步骤 2.1: 提取 XML 输入
        // extractXmlInputForToolCalls() 会：
        // - 优先查找本次请求的随机触发标记 (session.tool_bridge_trigger)
        // - 如果找到触发标记，返回从触发标记开始的子串
        // - 如果没找到，尝试查找 <function_calls> 标签作为兜底
        std::string xmlInput = extractXmlInputForToolCalls(session, text);

        if (xmlInput.empty()) {
            // 没有找到工具调用 XML，全部当作普通文本
            textContent = text;
            // 记录 TOOL_BRIDGE 警告：未找到 XML 工具调用
            if (!session.tools.isNull() && session.tools.isArray() && session.tools.size() > 0) {
                // 只有在有工具定义时才记录警告
                recordWarnStat(
                    session,
                    metrics::Domain::TOOL_BRIDGE,
                    metrics::EventType::TOOLBRIDGE_XML_NOT_FOUND,
                    "No XML tool calls found in response",
                    Json::Value(),
                    text.substr(0, std::min(text.size(), size_t(1024)))  // 截取前 1KB 作为原始片段
                );
            }
        } else {
            // 步骤 2.2: 规范化 XML（处理特殊空格、换行符等）
            xmlInput = normalizeBridgeXml(std::move(xmlInput));
            
            // 步骤 2.3: 确定是否使用 Sentinel 严格匹配
            // 【Sentinel 机制说明】
            // 每次请求会生成一个随机的触发标记（如 <Function_Ab1c_Start/>），
            // 只有包含该标记的 XML 块才会被解析，避免误解析历史消息或示例中的 XML。
            std::string expectedSentinel = "";
            if (!session.tool_bridge_trigger.empty() &&
                xmlInput.find(session.tool_bridge_trigger) != std::string::npos) {
                // XML 中包含本次请求的触发标记，启用严格匹配
                expectedSentinel = session.tool_bridge_trigger;
            }
            
            // 步骤 2.4: 解析 XML 工具调用
            // parseXmlToolCalls() 会：
            // - 使用 XmlTagToolCallCodec 解析 <function_calls>/<function_call> 结构
            // - 提取 <tool> 和 <args_json> 内容
            // - 将解析结果填充到 toolCalls 向量
            // - 非工具调用的文本会填充到 textContent
            parseXmlToolCalls(xmlInput, textContent, toolCalls, expectedSentinel);
        }

        // 步骤 2.5: 强制工具调用兜底
        // 如果 tool_choice=required 但上游没有返回工具调用，
        // 尝试根据用户输入自动生成一个工具调用
        if (toolCalls.empty()) {
            generateForcedToolCall(session, toolCalls, textContent);
        }
    }

    // ==================== 步骤 3: 参数形状规范化 ====================
    // normalizeToolCallArguments() 会：
    // - 修复常见的参数格式问题（如 files:["path"] → files:[{path:"path"}]）
    // - 处理参数别名（如 paths → files）
    // - 填充缺失的可选字段默认值
    // - 规范化 ask_followup_question 的 mode 字段
    normalizeToolCallArguments(session, toolCalls);

    // ==================== 步骤 4: Schema 校验 + 过滤无效调用 ====================
    // 【这是防止 "missing nativeArgs" 错误的关键步骤】
    //
    // 【问题背景】
    // 当上游模型输出不完整的工具调用（如参数 JSON 截断、必填字段缺失）时，
    // 客户端执行层会报错 "Invalid tool call: missing nativeArgs"。
    //
    // 【解决方案】
    // 在发送给客户端之前，使用 ToolCallValidator 进行多层校验：
    // - Stage A: 工具名存在性 + JSON 可解析 + required 字段 + 类型匹配
    // - Stage B: 关键字段非空（path/diff/content 等）
    // - Stage C: 校验失败后的降级策略（按客户端类型）
    {
        // 获取工具定义（优先使用 tools_raw，它保存了原始的客户端工具定义）
        const Json::Value& toolDefs =
            (!session.tools_raw.isNull() && session.tools_raw.isArray() && session.tools_raw.size() > 0)
                ? session.tools_raw
                : session.tools;
        
        // 只有在有工具调用且有工具定义时才进行校验
        if (!toolCalls.empty() && toolDefs.isArray() && toolDefs.size() > 0) {
            // 创建校验器，传入工具定义和客户端类型
            // 客户端类型用于选择不同的关键字段集合：
            // - RooCode/Kilo-Code: 使用完整的关键字段集合
            // - 其他客户端: 使用最小关键字段集合
            toolcall::ToolCallValidator validator(toolDefs, clientType);
            
            // 用于收集被丢弃的工具调用信息（供降级策略使用）
            std::string discardedText;
            
            // 【校验模式选择】
            // 根据客户端类型自动选择推荐的校验模式：
            // - RooCode/Kilo-Code: Relaxed 模式（校验关键字段）
            // - 其他客户端: None 模式（不校验，信任 AI 输出）
            //
            // 原因：
            // 1. Roo/Kilo 客户端对工具调用格式要求严格，需要提前过滤明显错误
            // 2. 其他客户端使用宽松策略，避免误报（如 mode 字段问题）
            // 3. prompt 中已经明确告诉 AI 哪些参数是 required
            toolcall::ValidationMode validationMode = toolcall::getRecommendedValidationMode(clientType);
            
            // 执行校验并过滤无效的工具调用
            // filterInvalidToolCalls() 会：
            // - 遍历所有工具调用，逐个校验
            // - 移除不通过校验的工具调用
            // - 返回被移除的数量
            size_t removedCount = validator.filterInvalidToolCalls(toolCalls, discardedText, validationMode);
            
            if (removedCount > 0) {
                LOG_WARN << "[GenerationService] 通过 Schema 校验过滤了 " << removedCount
                         << " 个无效的工具调用";
                
                // 记录 TOOL_BRIDGE 警告：校验过滤了无效的工具调用
                Json::Value filterDetail;
                filterDetail["removed_count"] = static_cast<Json::UInt64>(removedCount);
                filterDetail["validation_mode"] = static_cast<int>(validationMode);
                recordWarnStat(
                    session,
                    metrics::Domain::TOOL_BRIDGE,
                    metrics::EventType::TOOLBRIDGE_VALIDATION_FILTERED,
                    "Filtered " + std::to_string(removedCount) + " invalid tool calls",
                    filterDetail,
                    discardedText.substr(0, std::min(discardedText.size(), size_t(2048)))
                );
                
                // Stage C: 根据客户端类型应用降级策略
                // 如果所有工具调用都被过滤掉了，需要决定如何处理
                if (toolCalls.empty()) {
                    // applyValidationFallback() 会：
                    // - 非严格客户端：仅丢弃，保留文本输出
                    // - 严格客户端（Roo/Kilo）：将文本包装为 attempt_completion
                    toolcall::applyValidationFallback(clientType, toolCalls, textContent, discardedText);
                    
                    // 记录 TOOL_BRIDGE 警告：应用了降级策略
                    recordWarnStat(
                        session,
                        metrics::Domain::TOOL_BRIDGE,
                        metrics::EventType::TOOLBRIDGE_VALIDATION_FALLBACK_APPLIED,
                        "Applied validation fallback for client: " + clientType
                    );
                }
            }
        }
    }

    // ==================== 步骤 5: 严格客户端规则（仅 Roo/Kilo）====================
    // 【规则说明】
    // Roo/Kilo 客户端要求每次响应必须且只能包含 1 个工具调用。
    //
    // applyStrictClientRules() 会：
    // - 如果没有工具调用但有文本 → 包装为 attempt_completion
    // - 如果有多个工具调用 → 只保留第一个
    // - 如果有工具调用 → 清空文本内容（避免重复输出）
    if (strictToolClient) {
        // 记录应用严格客户端规则前的状态
        size_t toolCallsBeforeStrict = toolCalls.size();
        bool hadTextBeforeStrict = !textContent.empty();
        
        applyStrictClientRules(clientType, textContent, toolCalls);
        
        // 如果规则导致了变化，记录统计
        if (toolCallsBeforeStrict != toolCalls.size() ||
            (hadTextBeforeStrict && textContent.empty() && !toolCalls.empty())) {
            Json::Value strictDetail;
            strictDetail["original_tool_calls"] = static_cast<Json::UInt64>(toolCallsBeforeStrict);
            strictDetail["final_tool_calls"] = static_cast<Json::UInt64>(toolCalls.size());
            strictDetail["text_cleared"] = hadTextBeforeStrict && textContent.empty();
            recordWarnStat(
                session,
                metrics::Domain::TOOL_BRIDGE,
                metrics::EventType::TOOLBRIDGE_STRICT_CLIENT_RULE_APPLIED,
                "Applied strict client rules for: " + clientType,
                strictDetail
            );
        }
    }

    // ==================== 步骤 7: 零宽字符会话ID嵌入 ====================
    // 【功能说明】
    // 使用零宽字符将会话ID嵌入到响应文本中，客户端下次请求时可以提取该ID实现续聊。
    //
    // 【特殊处理】
    // 对于 tool_calls 场景，部分客户端在收到 finish_reason="tool_calls" 后
    // 会停止处理后续内容，因此需要在 tool_calls 之前发送会话ID。
    auto& sessionManager = *chatSession::getInstance();

    if (sessionManager.isZeroWidthMode() && !session.curConversationId.empty()) {
        const std::string clientType = safeJsonAsString(session.client_info.get("client_type", ""), "");
        if (!toolCalls.empty() && clientType == "claudecode") {
            // Claude Code 客户端 + 有工具调用：单独发送零宽会话ID
            std::string zwOnly = chatSession::embedSessionIdInText("", session.curConversationId);
            if (!zwOnly.empty()) {
                generation::OutputTextDone zwDone;
                zwDone.text = zwOnly;
                zwDone.index = 0;
                sink.onEvent(zwDone);
                LOG_INFO << "[生成服务] 已在 tool_calls 前发送零宽会话ID: " << session.curConversationId;
            }
        } else {
            // 其他情况：在文本末尾嵌入会话ID
            textContent = chatSession::embedSessionIdInText(textContent, session.curConversationId);
            LOG_INFO << "[生成服务] 已在响应中嵌入会话ID: " << session.curConversationId;
        }
    }

    // ==================== 步骤 8: 发送事件 ====================
    // 【事件发送顺序】
    // 1. ToolCallDone 事件（如果有工具调用）
    // 2. OutputTextDone 事件（如果有文本内容）
    // 3. Completed 事件（标记响应结束）
    
    // 发送所有工具调用事件
    for (const auto& tc : toolCalls) {
        sink.onEvent(tc);
    }

    // 发送文本内容事件（如果有）
    if (!textContent.empty()) {
        generation::OutputTextDone textDone;
        textDone.text = textContent;
        textDone.index = 0;
        sink.onEvent(textDone);
    }

    // 发送完成事件
    // finish_reason: "stop"（普通文本结束）或 "tool_calls"（工具调用结束）
    generation::Completed completed;
    completed.finishReason = toolCalls.empty() ? "stop" : "tool_calls";
    sink.onEvent(completed);
}

void GenerationService::emitError(
    generation::ErrorCode code,
    const std::string& message,
    IResponseSink& sink
) {
    generation::Error error;
    error.code = code;
    error.message = message;
    sink.onEvent(error);
}

std::string GenerationService::sanitizeOutput(
    const Json::Value& clientInfo,
    const std::string& text
) {
    return ClientOutputSanitizer::sanitize(clientInfo, text);
}

void GenerationService::processOutputWithBridge(
    const std::string& text,
    bool supportsToolCalls,
    const session_st& session,
    IResponseSink& sink
) {
    std::string textContent;
    std::vector<generation::ToolCallDone> toolCalls;

    if (supportsToolCalls) {
        LOG_INFO << "[GenerationService] 通道支持 tool calls，跳过 Bridge 处理";
        textContent = text;
    } else {
        // 使用 ToolCallBridge 处理文本中的工具调用
        LOG_INFO << "[GenerationService] 通道不支持 tool calls，使用 ToolCallBridge 处理输出";
        
        auto bridge = toolcall::createToolCallBridge(supportsToolCalls);
        auto codec = toolcall::createXmlTagToolCallCodec();
        bridge->setTextCodec(codec);
        
        std::vector<toolcall::ToolCallEvent> events;
        bridge->transformResponseChunk(text, events);
        bridge->flushResponse(events);
        
        // 将事件转换回文本或 ToolCall 事件
        for (const auto& event : events) {
            switch (event.type) {
                case toolcall::EventType::Text:
                    textContent += event.text;
                    break;
                case toolcall::EventType::ToolCallEnd: {
                    LOG_INFO << "[GenerationService] 工具调用结束: " << event.toolName;
                    generation::ToolCallDone tc;
                    tc.id = event.toolCallId;
                    tc.name = event.toolName;
                    tc.arguments = event.argumentsDelta; // 这里是完整参数
                    tc.index = toolCalls.size();
                    toolCalls.push_back(tc);
                    // Also trigger separate ToolCallDone event
                    sink.onEvent(tc);
                    break;
                }
                case toolcall::EventType::ToolCallBegin:
                case toolcall::EventType::ToolCallArgsDelta:
                    // 这些是流式过程中的中间状态，对于非流式聚合结果，我们关注 End 事件
                    break;
                case toolcall::EventType::Error:
                    LOG_WARN << "[GenerationService] 解析错误: " << event.errorMessage;
                    break;
            }
        }
    }
    
    // 3. 如果使用零宽字符模式，在响应末尾嵌入会话ID
    auto& sessionManager = *chatSession::getInstance();
    if (sessionManager.isZeroWidthMode() && !session.curConversationId.empty()) {
        textContent = chatSession::embedSessionIdInText(textContent, session.curConversationId);
        LOG_INFO << "[生成服务] 已在响应中嵌入会话ID: " << session.curConversationId;
    }
    
    // 发送 OutputTextDone 事件 (如果有文本)
    if (!textContent.empty()) {
        generation::OutputTextDone textDone;
        textDone.text = textContent;
        textDone.index = 0;
        sink.onEvent(textDone);
    }
    
    // 发送 Completed 事件
    generation::Completed completed;
    completed.finishReason = toolCalls.empty() ? "stop" : "tool_calls";
    sink.onEvent(completed);
}

bool GenerationService::getChannelSupportsToolCalls(const std::string& channelName) {
    // 从数据库获取通道信息
    auto channelManager = ChannelDbManager::getInstance();
    Channelinfo_st channelInfo;
    
    if (channelManager->getChannel(channelName, channelInfo)) {
        LOG_INFO << "[GenerationService] 通道 " << channelName
                  << " supportsToolCalls: " << channelInfo.supportsToolCalls;
        return channelInfo.supportsToolCalls;
    }
    
    // 默认返回 true（保守策略，避免破坏现有行为）
    LOG_WARN << "[GenerationService] 未找到通道 " << channelName << "，默认支持 tool calls";
    return true;
}

// ========== emitResultEvents 辅助函数实现 ==========

/**
 * @brief 解析 XML 格式的工具调用
 *
 * 【功能说明】
 * 从 XML 文本中解析出工具调用信息。这是 Bridge 模式的核心解析函数。
 *
 * 【XML 格式示例】
 * <Function_Ab1c_Start/>          ← 触发标记（Sentinel）
 * <function_calls>
 *   <function_call>
 *     <tool>read_file</tool>
 *     <args_json><![CDATA[{"path":"src/main.cpp"}]]></args_json>
 *   </function_call>
 * </function_calls>
 *
 * 【Sentinel 机制】
 * - 每次请求生成唯一的触发标记（如 <Function_Ab1c_Start/>）
 * - 只有包含该标记的 XML 块才会被解析
 * - 防止误解析历史消息或示例中的 XML
 *
 * @param xmlInput 待解析的 XML 文本
 * @param outTextContent [输出] 非工具调用的普通文本
 * @param outToolCalls [输出] 解析出的工具调用列表
 * @param sentinel 期望的触发标记，为空则不做严格匹配
 */
void GenerationService::parseXmlToolCalls(
    const std::string& xmlInput,
    std::string& outTextContent,
    std::vector<generation::ToolCallDone>& outToolCalls,
    const std::string& sentinel
) {
    LOG_INFO << "[GenerationService] 解析 XML 格式 tool calls, sentinel: " << (sentinel.empty() ? "NONE" : sentinel);
    LOG_INFO << "[ToolParse] xmlInput.size=" << xmlInput.size()
              << " has </args_json>=" << (xmlInput.find("</args_json>") != std::string::npos)
              << " has </function_call>=" << (xmlInput.find("</function_call>") != std::string::npos)
              << " has </function_calls>=" << (xmlInput.find("</function_calls>") != std::string::npos);

    // 创建 ToolCallBridge 和 XmlTagToolCallCodec
    // - ToolCallBridge: 负责协调请求/响应的转换
    // - XmlTagToolCallCodec: 负责 XML 格式的编解码
    auto bridge = toolcall::createToolCallBridge(false);
    auto codec = toolcall::createXmlTagToolCallCodec();

    // 设置 Sentinel（触发标记）
    // 如果设置了 Sentinel，Codec 只会解析包含该标记的 XML 块
    if (!sentinel.empty()) {
        codec->setSentinel(sentinel);
    }
    bridge->setTextCodec(codec);

    // 执行解析
    std::vector<toolcall::ToolCallEvent> events;
    bridge->transformResponseChunk(xmlInput, events);  // 增量解析
    bridge->flushResponse(events);                      // 刷新剩余内容

    // 处理解析事件
    for (const auto& event : events) {
        switch (event.type) {
            case toolcall::EventType::Text:
                // 普通文本事件：累积到 outTextContent
                outTextContent += event.text;
                break;
                
            case toolcall::EventType::ToolCallEnd: {
                // 工具调用结束事件：创建 ToolCallDone 结构
                LOG_INFO << "[GenerationService] 工具调用结束: " << event.toolName;
                generation::ToolCallDone tc;
                tc.id = event.toolCallId;           // 工具调用 ID（如 call_xxx）
                tc.name = event.toolName;           // 工具名称（如 read_file）
                tc.arguments = event.argumentsDelta; // 参数 JSON 字符串
                tc.index = outToolCalls.size();     // 在列表中的索引
                outToolCalls.push_back(tc);
                break;
            }
            
            case toolcall::EventType::ToolCallBegin:
            case toolcall::EventType::ToolCallArgsDelta:
                // 这些是流式解析的中间状态，非流式场景下忽略
                break;
                
            case toolcall::EventType::Error:
                // 解析错误：记录日志但不中断处理
                LOG_WARN << "[GenerationService] 解析错误: " << event.errorMessage
                         << " (xmlInput.size=" << xmlInput.size() << ")";
                {
                    // 输出 XML 尾部用于调试
                    const size_t kTail = 320;
                    const std::string tail =
                        xmlInput.size() > kTail ? xmlInput.substr(xmlInput.size() - kTail) : xmlInput;
                    LOG_INFO << "[GenerationService] xmlInput tail (truncated): " << tail;
                }
                break;
        }
    }

    // 【重要】如果解析出了工具调用，清空文本内容
    // 避免触发标记、空白字符等被当作普通文本输出
    if (!outToolCalls.empty()) {
        outTextContent.clear();
    }
}

void GenerationService::generateForcedToolCall(
    const session_st& session,
    std::vector<generation::ToolCallDone>& outToolCalls,
    std::string& outTextContent
) {
    std::string toolChoice = "auto";
    std::string forcedToolName;

    if (!session.toolChoice.empty()) {
        if (session.toolChoice.front() == '{') {
            Json::Value tc;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream iss(session.toolChoice);
            if (Json::parseFromStream(builder, iss, &tc, &errors) && tc.isObject()) {
                if (tc.get("type", "").asString() == "function" && tc.isMember("function") && tc["function"].isObject()) {
                    forcedToolName = tc["function"].get("name", "").asString();
                    if (!forcedToolName.empty()) {
                        toolChoice = "required";
                    }
                }
            }
        } else {
            toolChoice = toLowerStr(session.toolChoice);
        }
    }

    const bool mustCallTool = (toolChoice == "required") || !forcedToolName.empty();
    const Json::Value& toolDefs =
        (!session.tools_raw.isNull() && session.tools_raw.isArray() && session.tools_raw.size() > 0)
            ? session.tools_raw
            : session.tools;

    if (!mustCallTool || !toolDefs.isArray() || toolDefs.size() == 0) {
        return;
    }

    auto startsWith = [](const std::string& s, const std::string& prefix) -> bool {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    };
    auto endsWith = [](const std::string& s, const std::string& suffix) -> bool {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    auto stripTrailingPunct = [&](std::string s) -> std::string {
        s = trimWhitespace(std::move(s));
        static const std::vector<std::string> suffixes = {
            "？", "?", "！", "!", "。", ".", "，", ",", "；", ";", ":", "："
        };
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& suf : suffixes) {
                if (endsWith(s, suf)) {
                    s.erase(s.size() - suf.size());
                    s = trimWhitespace(std::move(s));
                    changed = true;
                }
            }
        }
        return s;
    };
    auto takeLastToken = [&](std::string s) -> std::string {
        s = trimWhitespace(std::move(s));
        size_t pos = s.find_last_of(" \t\r\n");
        if (pos == std::string::npos) return s;
        return trimWhitespace(s.substr(pos + 1));
    };

    auto extractLocationLike = [&](std::string input) -> std::string {
        std::string s = stripTrailingPunct(trimWhitespace(std::move(input)));

        // CN weather-like patterns: "...的天气..."
        size_t pos = s.find("天气");
        if (pos == std::string::npos) pos = s.find("气温");
        if (pos != std::string::npos) {
            std::string before = stripTrailingPunct(s.substr(0, pos));

            // remove common prefixes
            static const std::vector<std::string> prefixes = {
                "请帮我查一下", "请帮我查下", "请帮我查查", "请帮我查", "帮我查一下", "帮我查下", "帮我查查", "帮我查",
                "麻烦查一下", "麻烦查下", "麻烦查查", "麻烦查", "查一下", "查下", "查查", "查询", "看看", "看一下", "请问", "问一下"
            };
            before = trimWhitespace(std::move(before));
            bool removed = true;
            while (removed) {
                removed = false;
                for (const auto& p : prefixes) {
                    if (startsWith(before, p)) {
                        before = trimWhitespace(before.substr(p.size()));
                        removed = true;
                        break;
                    }
                }
            }

            // remove trailing particles/time words
            static const std::vector<std::string> timeSuffixes = {"今天", "明天", "后天", "现在", "当前", "目前"};
            before = stripTrailingPunct(trimWhitespace(std::move(before)));
            for (const auto& t : timeSuffixes) {
                if (endsWith(before, t)) {
                    before.erase(before.size() - t.size());
                    before = trimWhitespace(std::move(before));
                }
            }

            if (endsWith(before, "的")) {
                before.erase(before.size() - std::string("的").size());
                before = trimWhitespace(std::move(before));
            }

            before = takeLastToken(std::move(before));
            if (!before.empty()) return before;
        }

        // EN: "weather in X"
        std::string lower = toLowerStr(s);
        const std::string needle = "weather in ";
        size_t p = lower.find(needle);
        if (p != std::string::npos) {
            std::string loc = stripTrailingPunct(trimWhitespace(s.substr(p + needle.size())));
            if (!loc.empty()) return loc;
        }

        return stripTrailingPunct(trimWhitespace(std::move(s)));
    };

    // pick tool
    std::string toolName = forcedToolName;
    Json::Value toolObj;
    if (toolName.empty() && toolDefs.size() == 1) {
        toolObj = toolDefs[0];
        if (toolObj.isObject() && toolObj.get("type", "").asString() == "function") {
            toolName = toolObj["function"].get("name", "").asString();
        }
    }
    if (toolName.empty()) {
        // Fallback: pick the first function tool
        for (const auto& t : toolDefs) {
            if (!t.isObject()) continue;
            if (t.get("type", "").asString() != "function") continue;
            toolName = t["function"].get("name", "").asString();
            toolObj = t;
            if (!toolName.empty()) break;
        }
    } else {
        // locate the forced tool object
        for (const auto& t : toolDefs) {
            if (!t.isObject()) continue;
            if (t.get("type", "").asString() != "function") continue;
            if (t["function"].get("name", "").asString() == toolName) {
                toolObj = t;
                break;
            }
        }
    }

    if (toolName.empty()) {
        return;
    }

    Json::Value args(Json::objectValue);
    const std::string srcText = session.requestmessage_raw.empty() ? session.requestmessage : session.requestmessage_raw;

    std::vector<std::string> requiredParams;
    if (toolObj.isObject() && toolObj.isMember("function") && toolObj["function"].isObject()) {
        const auto& schema = toolObj["function"]["parameters"];
        if (schema.isObject()) {
            const auto& required = schema["required"];
            if (required.isArray()) {
                for (const auto& r : required) {
                    if (r.isString()) requiredParams.push_back(r.asString());
                }
            }
            if (requiredParams.empty() && schema.isMember("properties") && schema["properties"].isObject()) {
                for (const auto& p : schema["properties"].getMemberNames()) {
                    requiredParams.push_back(p);
                }
            }
        }
    }

    auto isLocationParam = [&](const std::string& name) -> bool {
        const std::string n = toLowerStr(name);
        return n.find("location") != std::string::npos ||
               n.find("city") != std::string::npos ||
               n.find("place") != std::string::npos ||
               n.find("address") != std::string::npos ||
               n.find("region") != std::string::npos;
    };

    for (const auto& p : requiredParams) {
        if (p.empty()) continue;
        std::string v;
        if (isLocationParam(p)) {
            v = extractLocationLike(srcText);
        } else {
            v = trimWhitespace(srcText);
        }
        args[p] = v;
    }

    generation::ToolCallDone tc;
    tc.id = generateFallbackToolCallId();
    tc.name = toolName;
    tc.index = 0;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    tc.arguments = Json::writeString(writer, args);

    outToolCalls.push_back(tc);
    outTextContent.clear();

    LOG_WARN << "[GenerationService] 上游未返回 tool call，已根据 tool_choice=required 生成兜底 tool call: " << toolName;
    
    // 记录 TOOL_BRIDGE 警告：生成了强制工具调用
    Json::Value forcedDetail;
    forcedDetail["tool_name"] = toolName;
    forcedDetail["tool_choice"] = session.toolChoice;
    forcedDetail["generated_args"] = args;
    recordWarnStat(
        session,
        metrics::Domain::TOOL_BRIDGE,
        metrics::EventType::TOOLBRIDGE_FORCED_TOOLCALL_GENERATED,
        "Generated forced tool call: " + toolName,
        forcedDetail,
        srcText.substr(0, std::min(srcText.size(), size_t(512))),
        toolName
    );
}

/**
 * @brief 规范化工具调用参数的形状
 *
 * 【功能说明】
 * 上游模型输出的参数格式可能与客户端期望的 JSONSchema 不完全匹配。
 * 此函数负责将参数转换为正确的形状，避免客户端解析失败。
 *
 * 【常见问题示例】
 * 1. 数组元素类型错误：
 *    - 上游输出: files: ["src/main.cpp", "src/util.cpp"]
 *    - 期望格式: files: [{path: "src/main.cpp"}, {path: "src/util.cpp"}]
 *
 * 2. 参数别名：
 *    - 上游输出: paths: ["src/main.cpp"]
 *    - 期望格式: files: [{path: "src/main.cpp"}]
 *
 * 3. 缺失可选字段：
 *    - 上游输出: follow_up: [{text: "Yes"}]
 *    - 期望格式: follow_up: [{text: "Yes", mode: ""}]
 *
 * @param session 会话状态，包含工具定义
 * @param toolCalls [输入/输出] 待规范化的工具调用列表
 */
void GenerationService::normalizeToolCallArguments(
    const session_st& session,
    std::vector<generation::ToolCallDone>& toolCalls
) {
    if (toolCalls.empty()) {
        return;
    }

    const Json::Value& toolDefs =
        (!session.tools_raw.isNull() && session.tools_raw.isArray() && session.tools_raw.size() > 0)
            ? session.tools_raw
            : session.tools;

    auto findToolObj = [&](const std::string& toolName) -> const Json::Value* {
        if (!toolDefs.isArray()) return nullptr;
        for (const auto& t : toolDefs) {
            if (!t.isObject()) continue;
            if (t.get("type", "").asString() != "function") continue;
            const auto& func = t["function"];
            if (!func.isObject()) continue;
            if (func.get("name", "").asString() == toolName) {
                return &t;
            }
        }
        return nullptr;
    };

    auto normalizeArrayOfObjectParam = [&](Json::Value& args, const std::string& paramName, const Json::Value& paramSchema) {
        if (!args.isObject() || !args.isMember(paramName) || !args[paramName].isArray()) {
            return;
        }
        if (!paramSchema.isObject() || paramSchema.get("type", "").asString() != "array") {
            return;
        }
        const auto& items = paramSchema["items"];
        if (!items.isObject() || items.get("type", "").asString() != "object") {
            return;
        }

        std::vector<std::string> requiredKeys;
        const auto& req = items["required"];
        if (req.isArray()) {
            for (const auto& r : req) {
                if (r.isString()) requiredKeys.push_back(r.asString());
            }
        }
        if (requiredKeys.empty() && items.isMember("properties") && items["properties"].isObject()) {
            for (const auto& p : items["properties"].getMemberNames()) {
                requiredKeys.push_back(p);
            }
        }
        if (requiredKeys.empty()) {
            return;
        }

        const Json::Value* itemProps = nullptr;
        if (items.isMember("properties") && items["properties"].isObject()) {
            itemProps = &items["properties"];
        }
        auto getPropType = [&](const std::string& key) -> std::string {
            if (!itemProps || !itemProps->isObject()) return "";
            if (!itemProps->isMember(key) || !(*itemProps)[key].isObject()) return "";
            return (*itemProps)[key].get("type", "").asString();
        };
        auto hasKey = [&](const std::string& key) -> bool {
            for (const auto& k : requiredKeys) {
                if (k == key) return true;
            }
            return false;
        };
        auto pickStringKey = [&]() -> std::string {
            // Common nested-array patterns emitted by upstreams:
            // - read_file.files: ["path1","path2"]  -> [{"path":"path1"},...]
            // - ask_followup_question.follow_up: ["..."] -> [{"text":"..."}]
            if (paramName == "files") return "path";
            if (hasKey("text")) return "text";
            if (hasKey("path")) return "path";
            if (hasKey("name")) return "name";
            if (hasKey("id")) return "id";
            return requiredKeys[0];
        };

        Json::Value out(Json::arrayValue);
        for (const auto& el : args[paramName]) {
            Json::Value obj;
            if (el.isString()) {
                obj = Json::Value(Json::objectValue);
                obj[pickStringKey()] = el.asString();
            } else if (el.isObject()) {
                obj = el;
            } else {
                continue;
            }

            // Fill common required keys with safe defaults when missing
            for (const auto& k : requiredKeys) {
                if (!obj.isMember(k)) {
                    if (k == "mode") {
                        obj[k] = "";
                    }
                }
            }

            // Special-case: read_file file object expects {path:string}
            if (paramName == "files") {
                if (!obj.isMember("path")) {
                    if (obj.isMember("file") && obj["file"].isString()) obj["path"] = obj["file"].asString();
                    else if (obj.isMember("name") && obj["name"].isString()) obj["path"] = obj["name"].asString();
                }
            }

            // Drop invalid/incomplete entries to avoid clients crashing on `undefined` paths, etc.
            bool valid = true;
            for (const auto& k : requiredKeys) {
                if (k == "mode") {
                    if (!obj.isMember(k) || !obj[k].isString()) obj[k] = "";
                    continue;
                }
                if (!obj.isMember(k) || obj[k].isNull()) {
                    valid = false;
                    break;
                }
                const std::string t = getPropType(k);
                if (t == "string" && !obj[k].isString()) {
                    Json::StreamWriterBuilder writer;
                    writer["indentation"] = "";
                    obj[k] = Json::writeString(writer, obj[k]);
                }
            }

            if (valid && paramName == "files") {
                if (!obj.isMember("path") || !obj["path"].isString() || obj["path"].asString().empty()) {
                    valid = false;
                }
            }

            if (valid) {
                out.append(obj);
            }
        }

        // Always replace with a normalized array (even if empty) to avoid downstream crashes.
        args[paramName] = out;
    };

    for (auto& tc : toolCalls) {
        if (tc.arguments.empty()) continue;

        Json::Value args;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream iss(tc.arguments);
        if (!Json::parseFromStream(builder, iss, &args, &errors) || !args.isObject()) {
            continue;
        }

        const Json::Value* toolObj = findToolObj(tc.name);
        if (!toolObj) {
            continue;
        }

        const auto& paramsSchema = (*toolObj)["function"]["parameters"];
        if (!paramsSchema.isObject() || !paramsSchema.isMember("properties") || !paramsSchema["properties"].isObject()) {
            continue;
        }

        // Common alias: some models use `paths` while schema expects `files` (read_file).
        if (tc.name == "read_file" && !args.isMember("files") && args.isMember("paths") && args["paths"].isArray()) {
            Json::Value files(Json::arrayValue);
            for (const auto& p : args["paths"]) {
                if (!p.isString()) continue;
                Json::Value obj(Json::objectValue);
                obj["path"] = p.asString();
                files.append(obj);
            }
            if (files.size() > 0) {
                args["files"] = files;
            }
        }

        const auto& props = paramsSchema["properties"];
        for (const auto& paramName : props.getMemberNames()) {
            const auto& paramSchema = props[paramName];
            const std::string type = paramSchema.get("type", "").asString();
            if (type == "array" && paramSchema.isMember("items") &&
                paramSchema["items"].isObject() &&
                paramSchema["items"].get("type", "").asString() == "object") {
                normalizeArrayOfObjectParam(args, paramName, paramSchema);
            }
        }

        // RooCode sometimes emits non-existent mode slugs (e.g. "single") for ask_followup_question.
        // Normalize unknown values to empty string (meaning "no mode switch") to keep clients stable.
        if (tc.name == "ask_followup_question" && args.isMember("follow_up") && args["follow_up"].isArray()) {
            for (Json::ArrayIndex i = 0; i < args["follow_up"].size(); ++i) {
                auto& item = args["follow_up"][i];
                if (!item.isObject()) continue;

                if (!item.isMember("mode") || !item["mode"].isString()) {
                    item["mode"] = "";
                    continue;
                }

                const std::string mode = toLowerStr(item["mode"].asString());
                if (!(mode.empty() || mode == "code" || mode == "ask" || mode == "architect")) {
                    item["mode"] = "";
                }
            }
        }

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        tc.arguments = Json::writeString(writer, args);
    }
}

/**
 * @brief 应用严格客户端规则（仅 Roo/Kilo）
 *
 * 【规则说明】
 * Roo/Kilo 客户端对响应格式有严格要求：
 * - 每次响应必须且只能包含 1 个工具调用
 * - 不允许纯文本响应（必须包装为 attempt_completion）
 * - 不允许多个工具调用（只保留第一个）
 *
 * 【处理逻辑】
 * 1. 如果没有工具调用但有文本 → 包装为 attempt_completion
 * 2. 如果有工具调用 → 清空文本内容（避免重复输出）
 * 3. 如果有多个工具调用 → 只保留第一个
 *
 * 【为什么需要这个规则？】
 * Roo/Kilo 客户端的工作流设计为：
 * - 每轮对话，AI 必须执行一个动作（工具调用）
 * - 客户端根据工具调用类型决定下一步操作
 * - 如果没有工具调用，客户端无法继续工作流
 *
 * @param clientType 客户端类型（用于日志）
 * @param textContent [输入/输出] 文本内容，处理后可能被清空
 * @param toolCalls [输入/输出] 工具调用列表，可能被修改
 */
void GenerationService::applyStrictClientRules(
    const std::string& clientType,
    std::string& textContent,
    std::vector<generation::ToolCallDone>& toolCalls
) {
    // 规则 1: 如果没有工具调用但有文本，包装为 attempt_completion
    // attempt_completion 是 Roo/Kilo 的"完成任务"工具，用于输出最终结果
    if (toolCalls.empty() && !textContent.empty()) {
        generation::ToolCallDone tc;
        tc.id = generateFallbackToolCallId();
        tc.name = "attempt_completion";
        tc.index = 0;

        Json::Value args(Json::objectValue);
        args["result"] = textContent;
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        tc.arguments = Json::writeString(writer, args);

        toolCalls.push_back(tc);
        LOG_WARN << "[GenerationService][" << clientType << "] 未检测到 tool call，已自动包装为 attempt_completion";

        // 清空文本内容（已包装到 attempt_completion 中）
        // 注意：零宽会话ID会在后续步骤单独嵌入
        textContent.clear();
    } else if (!toolCalls.empty()) {
        // // 规则 2: 如果有工具调用，清空文本内容
        // // 避免工具调用和文本同时输出导致客户端混淆
        // textContent.clear();
    }

    // 规则 3: 如果有多个工具调用，只保留第一个
    // Roo/Kilo 客户端每次只能处理一个工具调用
    if (toolCalls.size() > 1) {
        LOG_WARN << "[GenerationService][" << clientType << "] 检测到多个 tool call，已仅保留第一个以满足客户端约束";
        toolCalls.erase(toolCalls.begin() + 1, toolCalls.end());
    }
}

void GenerationService::transformRequestForToolBridge(session_st& session) {
    const std::string clientType = safeJsonAsString(session.client_info.get("client_type", ""), "");
    const bool strictToolClient = (clientType == "Kilo-Code" || clientType == "RooCode");

    // Kilo-Code / RooCode clients send a very long system prompt blob containing tool
    // instructions that are incompatible with our XML-tag bridge. Keep only the first
    // "intro" section so the upstream sees language/role guidance but not the full
    // client prompt blob.
    if (strictToolClient && !session.systemprompt.empty()) {
        const std::string marker = "\n\n====";
        size_t pos = session.systemprompt.find(marker);
        if (pos != std::string::npos) {
            session.systemprompt = session.systemprompt.substr(0, pos);
        }
    }

    auto encodeCompactToolList = [](const Json::Value& tools) -> std::string {
        if (!tools.isArray() || tools.empty()) {
            return "";
        }

        auto join = [](const std::vector<std::string>& items, const std::string& sep) -> std::string {
            std::ostringstream oss;
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) oss << sep;
                oss << items[i];
            }
            return oss.str();
        };

        // Collect required field names from schema
        auto getRequiredSet = [](const Json::Value& schema) -> std::unordered_set<std::string> {
            std::unordered_set<std::string> requiredSet;
            if (!schema.isObject()) return requiredSet;

            const auto& required = schema["required"];
            if (required.isArray()) {
                for (const auto& r : required) {
                    if (r.isString()) requiredSet.insert(r.asString());
                }
            }
            return requiredSet;
        };

        // Collect all property names from schema (for nested objects)
        auto collectSchemaKeys = [](const Json::Value& schema) -> std::vector<std::string> {
            std::vector<std::string> keys;
            if (!schema.isObject()) return keys;

            const auto& required = schema["required"];
            if (required.isArray()) {
                for (const auto& r : required) {
                    if (r.isString()) keys.push_back(r.asString());
                }
            }

            if (keys.empty() && schema.isMember("properties") && schema["properties"].isObject()) {
                for (const auto& p : schema["properties"].getMemberNames()) {
                    keys.push_back(p);
                }
            }

            return keys;
        };

        auto describeSchema = [&](const Json::Value& schema) -> std::string {
            if (!schema.isObject()) return "any";

            const std::string type = schema.get("type", "").asString();
            if (type == "array") {
                const auto& items = schema["items"];
                const std::string itemType = items.isObject() ? items.get("type", "").asString() : "";
                if (itemType == "object") {
                    const auto keys = collectSchemaKeys(items);
                    return "[{" + join(keys, ",") + "}]";
                }
                if (!itemType.empty()) {
                    return itemType + "[]";
                }
                return "array";
            }

            if (type == "object") {
                const auto keys = collectSchemaKeys(schema);
                return "{" + join(keys, ",") + "}";
            }

            return type.empty() ? "any" : type;
        };

        std::ostringstream oss;
        // Note: The header "API Definitions (* = required, ? = optional):" is now in the policy section
        for (const auto& tool : tools) {
            if (!tool.isObject()) continue;
            if (tool.get("type", "").asString() != "function") continue;

            const auto& func = tool["function"];
            if (!func.isObject()) continue;

            std::string name = func.get("name", "").asString();
            if (name.empty()) continue;

            std::vector<std::string> params;
            const auto& schema = func["parameters"];
            if (schema.isObject()) {
                const auto requiredSet = getRequiredSet(schema);
                const auto& props = schema["properties"];

                // Iterate over all properties, not just required ones
                if (props.isObject()) {
                    for (const auto& key : props.getMemberNames()) {
                        const bool isRequired = requiredSet.find(key) != requiredSet.end();
                        const std::string marker = isRequired ? "*" : "?";
                        
                        if (!props[key].isObject()) {
                            params.push_back(key + marker);
                            continue;
                        }
                        params.push_back(key + marker + ": " + describeSchema(props[key]));
                    }
                }
            }

            oss << "- " << name;
            if (!params.empty()) {
                oss << "(";
                for (size_t i = 0; i < params.size(); ++i) {
                    if (i) oss << ", ";
                    oss << params[i];
                }
                oss << ")";
            }
            oss << "\n";
        }
        return oss.str();
    };

    // Encode tool definitions for upstream.
    // Use a compact list to avoid huge descriptions (which can trigger upstream refusals
    // and also push the user request out of the context window).
    std::string toolDefinitions = encodeCompactToolList(session.tools);

    if (toolDefinitions.empty()) {
        LOG_WARN << "[GenerationService] 工具定义编码结果为空";
        return;
    }

    // Preserve originals for downstream parsing / fallbacks.
    if (session.requestmessage_raw.empty()) {
        session.requestmessage_raw = session.requestmessage;
    }
    if (session.tools_raw.isNull() || !session.tools_raw.isArray() || session.tools_raw.size() == 0) {
        session.tools_raw = session.tools;
    }

    // Tool choice parsing (supports both string and object encodings).
    auto normalizeLower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    std::string toolChoice = "auto";
    std::string forcedToolName;
    if (!session.toolChoice.empty()) {
        // JSON object form (e.g. {"type":"function","function":{"name":"xxx"}})
        if (!session.toolChoice.empty() && session.toolChoice.front() == '{') {
            Json::Value tc;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream iss(session.toolChoice);
            if (Json::parseFromStream(builder, iss, &tc, &errors) && tc.isObject()) {
                if (tc.get("type", "").asString() == "function" && tc.isMember("function") && tc["function"].isObject()) {
                    forcedToolName = tc["function"].get("name", "").asString();
                    if (!forcedToolName.empty()) {
                        toolChoice = "required";
                    }
                }
            }
        } else {
            toolChoice = normalizeLower(session.toolChoice);
        }
    }

    // Generate a per-request random trigger so we only parse tool calls that belong to
    // this request (avoids accidentally parsing example XML inside normal prose).
    session.tool_bridge_trigger = generateRandomTriggerSignal();
    const std::string& triggerSignal = session.tool_bridge_trigger;

    // Low-friction "task" prompt (English for better model compatibility):
    // Modern RLHF-aligned models may reject "system override" style tool instructions.
    // We present this as a formatting/extraction task and ask the model to output
    // a strictly delimited XML block (Toolify-style) when an action/tool call is needed.
    //
    // IMPORTANT: The wrapper tag <tool_instructions> is intentionally different from
    // XmlTagToolCallCodec's recognized tags (function_calls, function_call, tool, args_json)
    // to prevent the prompt itself from being misinterpreted as a tool call.
    {
        std::ostringstream policy;
        
        // Use English for better model stability with format constraints
        policy << "<tool_instructions>\n";
        
        if (strictToolClient) {
            policy << "Context: Software engineering collaboration\n";
            policy << "Task: Generate the next tool call instruction (tools are executed by an external system)\n";
            policy << "Goal: Based on the user request, select and output exactly 1 tool to call.\n";
            policy << "Note: Do NOT explain whether you have access/permissions. Do NOT ask the user to paste file contents. Use the listed tools directly (e.g., call read_file/list_files when you need files).\n";
        } else {
            policy << "Context: Software engineering collaboration\n";
            policy << "Task: Generate tool call instructions (tools are executed by an external system)\n";
            policy << "Goal: Based on the user request, select and output one or more tools to call.\n";
            policy << "Note: Do NOT explain whether you have access/permissions. Do NOT ask the user to paste file contents. Use the listed tools directly (e.g., call read_file/list_files when you need files).\n";
        }

        if (!forcedToolName.empty()) {
            policy << "Required tool for this task: " << forcedToolName << "\n";
        } else if (toolChoice == "required") {
            policy << "This task MUST output exactly 1 tool call (choose the most appropriate API).\n";
        } else if (strictToolClient) {
            policy << "Each response MUST output exactly 1 tool call.\n";
            policy << "If no other tool is needed, use attempt_completion to output the final result.\n";
        } else {
            policy << "Output tool calls in XML format ONLY when an API call is needed to complete the task; otherwise respond normally.\n";
        }

        policy << "\nRequirements:\n";
        policy << "- When outputting a tool call, output ONLY the XML (no explanations, no prefixes/suffixes, no markdown code blocks).\n";
        policy << "- You MUST follow this exact XML format (line 1 is the trigger marker, line 2 starts <function_calls>):\n";
        policy << triggerSignal << "\n";
        policy << "<function_calls>\n";
        policy << "  <function_call>\n";
        policy << "    <tool>TOOL_NAME</tool>\n";
        policy << "    <args_json><![CDATA[{\"PARAM_NAME\":\"VALUE\"}]]></args_json>\n";
        policy << "  </function_call>\n";
        policy << "</function_calls>\n";
        policy << "- <tool> MUST be a name that exists in the API definitions below.\n";
        policy << "- <args_json> MUST be a valid JSON object containing all required arguments for that tool.\n";
        policy << "- Parameter keys MUST exactly match the API definition (case-sensitive).\n";
        policy << "- Parameters marked with * are required and MUST be provided.\n";
        policy << "- Parameters marked with ? are optional.\n";

        policy << "\nAPI Definitions (* = required, ? = optional):\n";
        toolDefinitions = policy.str() + toolDefinitions;
        toolDefinitions += "</tool_instructions>\n\n";
    }

    LOG_INFO << "[GenerationService] 注入工具定义到 requestmessage, 长度: " << toolDefinitions.length();

    std::string originalInput = session.requestmessage;
    LOG_INFO << "工具定义:" << toolDefinitions;
    session.requestmessage = originalInput+ "\n\n" + toolDefinitions ;

    // 清除 tools，避免后续流程再次处理
    session.tools = Json::Value(Json::nullValue);
    
    // 记录 TOOL_BRIDGE 警告：注入了工具定义
    Json::Value injectDetail;
    injectDetail["tool_count"] = session.tools_raw.isArray() ? static_cast<Json::UInt64>(session.tools_raw.size()) : 0;
    injectDetail["trigger_signal"] = triggerSignal;
    injectDetail["tool_choice"] = toolChoice;
    injectDetail["forced_tool_name"] = forcedToolName;
    injectDetail["strict_client"] = strictToolClient;
    recordWarnStat(
        session,
        metrics::Domain::TOOL_BRIDGE,
        metrics::EventType::TOOLBRIDGE_TRANSFORM_INJECTED,
        "Injected tool definitions for bridge mode",
        injectDetail
    );
}

GenerationRequest GenerationService::buildRequest(
    const session_st& session,
    OutputProtocol protocol,
    bool stream
) {
    GenerationRequest req;
    
    req.sessionKey = session.curConversationId;
    req.previousKey = session.preConversationId;
    req.clientInfo = session.client_info;
    
    req.provider = session.selectapi;
    req.model = session.selectmodel;
    req.systemPrompt = session.systemprompt;
    
    req.currentInput = session.requestmessage;
    
    // 转换消息上下文
    for (const auto& msg : session.message_context) {
        std::string role = safeJsonAsString(msg.get("role", "user"), "user");
        std::string content = safeJsonAsString(msg.get("content", ""), "");
        
        if (role == "user") {
            req.messages.push_back(Message::user(content));
        } else if (role == "assistant") {
            req.messages.push_back(Message::assistant(content));
        } else if (role == "system") {
            req.messages.push_back(Message::system(content));
        }
    }
    
    req.stream = stream;
    req.protocol = protocol;
    
    return req;
}
