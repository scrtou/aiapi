#include "GenerationService.h"
#include "ClientOutputSanitizer.h"
#include "ToolCallBridge.h"
#include "XmlTagToolCallCodec.h"
#include <apiManager/ApiManager.h>
#include <apipoint/ProviderResult.h>
#include <tools/ZeroWidthEncoder.h>
#include <dbManager/channel/channelDbManager.h>
#include <drogon/drogon.h>
#include <iomanip>
#include <random>
#include <sstream>

using namespace drogon;
using namespace provider;
using namespace session;
using namespace error;

// ========== 共享 helper 实现 ==========

namespace {
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
	        LOG_DEBUG<<" supportsToolCalls"<<supportsToolCalls<<" session.tools.isNull()"<<session.tools.isNull()<<"session.tools.isArray()"<<session.tools.isArray()<<"session.tools.size()"<<session.tools.size();
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
            emitError(generation::ErrorCode::Cancelled, "Request cancelled", sink);
            sink.onClose();
            return AppError::cancelled("Request was cancelled");
        }
        
        // 3. 调用 Provider
        if (!executeProvider(session)) {
            emitError(
                generation::ErrorCode::ProviderError,
                session.responsemessage.get("error", "Provider error").asString(),
                sink
            );
            sink.onClose();
            return std::nullopt;  // Provider 错误已通过 sink 发送
        }
        
        // 4. 检查取消状态
        if (guard.isCancelled()) {
            LOG_INFO << "[生成服务] 调用提供者后请求被取消";
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
        
    } catch (const std::exception& e) {
        LOG_ERROR << "[生成服务] 执行门控会话异常: " << e.what();
        emitError(generation::ErrorCode::Internal, e.what(), sink);
    } catch (...) {
        LOG_ERROR << "[生成服务] 执行门控会话未知异常";
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
                session.responsemessage.get("error", "Provider error").asString(),
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
            session.responsemessage.get("error", "Provider error").asString(),
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
            session.responsemessage.get("error", "Provider error").asString(),
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

void GenerationService::emitResultEvents(const session_st& session, IResponseSink& sink) {
    // 获取输出文本
    std::string text = session.responsemessage.get("message", "").asString();
    const std::string clientType = session.client_info.get("client_type", "").asString();
    const bool strictToolClient = (clientType == "Kilo-Code" || clientType == "RooCode");
    
    // 1. 应用基础文本清洗（标签纠错、控制字符等）
    text = sanitizeOutput(session.client_info, text);
    
    // 2. 根据通道能力处理 tool calls（如果通道不支持，则使用 Bridge 解析）
    bool supportsToolCalls = getChannelSupportsToolCalls(session.selectapi);
    auto normalizeLower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const bool toolChoiceNone = (normalizeLower(session.toolChoice) == "none");

    // 将事件转换回文本或 ToolCall 事件
    std::string textContent;
    std::vector<generation::ToolCallDone> toolCalls;

    if (supportsToolCalls || toolChoiceNone) {
        textContent = text;
    } else {
        auto trimWs = [](std::string s) -> std::string {
            const auto start = s.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) return "";
            const auto end = s.find_last_not_of(" \t\n\r");
            return s.substr(start, end - start + 1);
        };

        auto stripMarkdownCodeFence = [&](std::string s) -> std::string {
            s = trimWs(std::move(s));
            if (s.rfind("```", 0) != 0) {
                return s;
            }

            // Find first newline after opening fence (``` or ```json)
            size_t firstNl = s.find('\n');
            if (firstNl == std::string::npos) {
                return s;
            }

            // Find closing fence from the end
            size_t lastFence = s.rfind("```");
            if (lastFence == std::string::npos || lastFence <= firstNl) {
                return s;
            }

            std::string inner = s.substr(firstNl + 1, lastFence - (firstNl + 1));
            return trimWs(std::move(inner));
        };

        auto findJsonObjectEnd = [](const std::string& s) -> size_t {
            // Find the end position (one past) of the first top-level JSON object.
            // Returns npos if incomplete or not starting with '{'.
            if (s.empty() || s[0] != '{') return std::string::npos;

            bool inString = false;
            bool escape = false;
            int depth = 0;

            for (size_t i = 0; i < s.size(); ++i) {
                char c = s[i];

                if (inString) {
                    if (escape) {
                        escape = false;
                        continue;
                    }
                    if (c == '\\') {
                        escape = true;
                        continue;
                    }
                    if (c == '"') {
                        inString = false;
                    }
                    continue;
                }

                if (c == '"') {
                    inString = true;
                    continue;
                }

                if (c == '{') {
                    depth++;
                } else if (c == '}') {
                    depth--;
                    if (depth == 0) {
                        return i + 1;
                    }
                }
            }

            return std::string::npos;
        };

        auto tryParseJsonToolCall = [&](const std::string& raw) -> bool {
            std::string s = stripMarkdownCodeFence(raw);
            s = trimWs(std::move(s));
            if (s.empty() || s[0] != '{') {
                return false;
            }

            size_t endPos = findJsonObjectEnd(s);
            if (endPos == std::string::npos) {
                return false;
            }

            // Reject any trailing non-whitespace to avoid false positives from examples in prose
            if (!trimWs(s.substr(endPos)).empty()) {
                return false;
            }

            Json::Value root;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream iss(s.substr(0, endPos));
            if (!Json::parseFromStream(builder, iss, &root, &errors) || !root.isObject()) {
                return false;
            }

            std::string toolName;
            Json::Value args(Json::objectValue);

            if (root.isMember("tool") && root["tool"].isString()) {
                toolName = root["tool"].asString();
            }
            if (root.isMember("args") && root["args"].isObject()) {
                args = root["args"];
            } else if (root.isMember("arguments") && root["arguments"].isObject()) {
                args = root["arguments"];
            }

            if (toolName.empty()) {
                return false;
            }

            generation::ToolCallDone tc;
            tc.id = generateFallbackToolCallId();
            tc.name = toolName;
            tc.index = 0;

            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            tc.arguments = Json::writeString(writer, args);

            toolCalls.push_back(tc);
            return true;
        };

        if (tryParseJsonToolCall(text)) {
            // Tool call captured; keep textContent empty
            textContent.clear();
        } else {
        // Avoid false positives:
        // Some model outputs include <function_calls> blocks as examples inside normal text.
        // Prefer parsing the Toolify-style trigger marker (<Function_..._Start/>) when present.
        // Otherwise, only parse when the response *starts* with a tool marker.
        auto ltrim = [](const std::string& s) -> std::string_view {
            size_t i = 0;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
            return std::string_view(s).substr(i);
        };
        auto startsWith = [](std::string_view s, std::string_view prefix) -> bool {
            return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
        };

	        std::string xmlCandidate = stripMarkdownCodeFence(text);

	        std::string xmlInput;
	        const std::string_view trimmed = ltrim(xmlCandidate);
	        auto findFunctionCallsPos = [](std::string_view s) -> size_t {
	            size_t p1 = s.find("<function_calls");
	            size_t p2 = s.find("<antml:function_calls");
	            if (p1 == std::string_view::npos) return p2;
	            if (p2 == std::string_view::npos) return p1;
	            return std::min(p1, p2);
	        };

	        if (!session.tool_bridge_trigger.empty()) {
	            // Only parse tool calls belonging to this request when the trigger matches.
	            // However, upstream UIs/models occasionally echo an older trigger marker. If the
	            // response starts with a tool marker, still parse it to avoid dropping valid calls.
	            size_t triggerPos = xmlCandidate.rfind(session.tool_bridge_trigger);
	            if (triggerPos != std::string::npos) {
	                xmlInput = xmlCandidate.substr(triggerPos);
	            } else {
	                const bool startsLikeToolCall =
	                    startsWith(trimmed, "<function_calls") ||
	                    startsWith(trimmed, "<antml:function_calls") ||
	                    startsWith(trimmed, "<Function_");
	                if (startsLikeToolCall) {
	                    if (startsWith(trimmed, "<Function_")) {
	                        // Require a real <function_calls> tag very early to reduce false positives.
	                        size_t fcPos = findFunctionCallsPos(trimmed);
	                        if (fcPos != std::string_view::npos && fcPos <= 512) {
	                            xmlInput = std::string(trimmed);
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
	            } else {
	                const bool looksLikeToolCall =
	                    startsWith(trimmed, "<function_calls") ||
	                    startsWith(trimmed, "<antml:function_calls");
	                if (looksLikeToolCall) {
	                    xmlInput = std::string(trimmed);
	                }
	            }
	        }

        if (xmlInput.empty()) {
            textContent = text;
        } else {
            LOG_INFO << "[GenerationService] 通道不支持 tool calls，使用 ToolCallBridge 处理输出";

            auto bridge = toolcall::createToolCallBridge(supportsToolCalls);
            auto codec = toolcall::createXmlTagToolCallCodec();
            bridge->setTextCodec(codec);

            std::vector<toolcall::ToolCallEvent> events;
            bridge->transformResponseChunk(xmlInput, events);
            bridge->flushResponse(events);

            for (const auto& event : events) {
                switch (event.type) {
                    case toolcall::EventType::Text:
                        textContent += event.text;
                        break;
                    case toolcall::EventType::ToolCallEnd: {
                        LOG_DEBUG << "[GenerationService] 工具调用结束: " << event.toolName;
                        generation::ToolCallDone tc;
                        tc.id = event.toolCallId;
                        tc.name = event.toolName;
                        tc.arguments = event.argumentsDelta; // 这里是完整参数
                        tc.index = toolCalls.size();
                        toolCalls.push_back(tc);
                        break;
                    }
                    case toolcall::EventType::ToolCallBegin:
                    case toolcall::EventType::ToolCallArgsDelta:
                        break;
                    case toolcall::EventType::Error:
                        LOG_WARN << "[GenerationService] 解析错误: " << event.errorMessage;
                        break;
                }
            }

            // When tool calls exist, suppress any stray text (e.g. trigger echoes, whitespace, etc.)
            if (!toolCalls.empty()) {
                textContent.clear();
            }
        }
        }
    }

    // tool_choice=required / forced tool fallback:
    // If the upstream refuses to follow our tool-bridge formatting, synthesize a tool call
    // locally to satisfy OpenAI semantics (and clients that require tool calls).
    if (!supportsToolCalls && toolCalls.empty()) {
        std::string toolChoice = "auto";
        std::string forcedToolName;
        if (!session.toolChoice.empty()) {
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

        const bool mustCallTool = (toolChoice == "required") || !forcedToolName.empty();
        const Json::Value& toolDefs =
            (!session.tools_raw.isNull() && session.tools_raw.isArray() && session.tools_raw.size() > 0)
                ? session.tools_raw
                : session.tools;

        if (mustCallTool && toolDefs.isArray() && toolDefs.size() > 0) {
            auto trimWs = [](std::string s) -> std::string {
                const auto start = s.find_first_not_of(" \t\n\r");
                if (start == std::string::npos) return "";
                const auto end = s.find_last_not_of(" \t\n\r");
                return s.substr(start, end - start + 1);
            };
            auto startsWith = [](const std::string& s, const std::string& prefix) -> bool {
                return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
            };
            auto endsWith = [](const std::string& s, const std::string& suffix) -> bool {
                return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
            };
            auto stripTrailingPunct = [&](std::string s) -> std::string {
                s = trimWs(std::move(s));
                static const std::vector<std::string> suffixes = {
                    "？", "?", "！", "!", "。", ".", "，", ",", "；", ";", ":", "："
                };
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (const auto& suf : suffixes) {
                        if (endsWith(s, suf)) {
                            s.erase(s.size() - suf.size());
                            s = trimWs(std::move(s));
                            changed = true;
                        }
                    }
                }
                return s;
            };
            auto takeLastToken = [&](std::string s) -> std::string {
                s = trimWs(std::move(s));
                size_t pos = s.find_last_of(" \t\r\n");
                if (pos == std::string::npos) return s;
                return trimWs(s.substr(pos + 1));
            };

            auto extractLocationLike = [&](std::string input) -> std::string {
                std::string s = stripTrailingPunct(trimWs(std::move(input)));

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
                    before = trimWs(std::move(before));
                    bool removed = true;
                    while (removed) {
                        removed = false;
                        for (const auto& p : prefixes) {
                            if (startsWith(before, p)) {
                                before = trimWs(before.substr(p.size()));
                                removed = true;
                                break;
                            }
                        }
                    }

                    // remove trailing particles/time words
                    static const std::vector<std::string> timeSuffixes = {"今天", "明天", "后天", "现在", "当前", "目前"};
                    before = stripTrailingPunct(trimWs(std::move(before)));
                    for (const auto& t : timeSuffixes) {
                        if (endsWith(before, t)) {
                            before.erase(before.size() - t.size());
                            before = trimWs(std::move(before));
                        }
                    }

                    if (endsWith(before, "的")) {
                        before.erase(before.size() - std::string("的").size());
                        before = trimWs(std::move(before));
                    }

                    before = takeLastToken(std::move(before));
                    if (!before.empty()) return before;
                }

                // EN: "weather in X"
                std::string lower = normalizeLower(s);
                const std::string needle = "weather in ";
                size_t p = lower.find(needle);
                if (p != std::string::npos) {
                    std::string loc = stripTrailingPunct(trimWs(s.substr(p + needle.size())));
                    if (!loc.empty()) return loc;
                }

                return stripTrailingPunct(trimWs(std::move(s)));
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

	            if (!toolName.empty()) {
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
                    const std::string n = normalizeLower(name);
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
                        v = trimWs(srcText);
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

                toolCalls.push_back(tc);
                textContent.clear();

                LOG_WARN << "[GenerationService] 上游未返回 tool call，已根据 tool_choice=required 生成兜底 tool call: " << toolName;
            }
        }
    }

    // Normalize tool call argument shapes according to the client-provided JSONSchema.
    // RooCode's tools often use nested schemas (e.g. read_file expects files:[{path:string}]),
    // while upstream may emit a simpler but invalid shape (e.g. files:[\"...\"]).
    if (!toolCalls.empty()) {
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
	                auto normalizeLower = [](std::string s) {
	                    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	                    return s;
	                };
	                for (Json::ArrayIndex i = 0; i < args["follow_up"].size(); ++i) {
	                    auto& item = args["follow_up"][i];
	                    if (!item.isObject()) continue;

	                    if (!item.isMember("mode") || !item["mode"].isString()) {
	                        item["mode"] = "";
	                        continue;
	                    }

	                    const std::string mode = normalizeLower(item["mode"].asString());
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

	    // Strict-client self-heal:
	    // Some upstream UIs/models refuse tool-use by saying "I don't have access to your filesystem".
	    // When that happens, they often list the exact file paths they want the user to paste.
	    // For tool-calling clients (Roo/Kilo), convert that refusal into a concrete `read_file` call
	    // so the client can fetch the files and continue the loop.
	    if (strictToolClient && toolCalls.empty() && !textContent.empty()) {
	        auto toLower = [](std::string s) {
	            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	            return s;
	        };

	        const std::string lower = toLower(textContent);
	        const bool looksLikeNoToolAccess =
	            (lower.find("no access") != std::string::npos) ||
	            (lower.find("don't have") != std::string::npos) ||
	            (lower.find("cannot") != std::string::npos) ||
	            (lower.find("can't") != std::string::npos) ||
	            (lower.find("file system") != std::string::npos) ||
	            (lower.find("filesystem") != std::string::npos) ||
	            (lower.find("workspace") != std::string::npos) ||
	            (textContent.find("没有") != std::string::npos) ||
	            (textContent.find("无法") != std::string::npos) ||
	            (textContent.find("做不到") != std::string::npos) ||
	            (textContent.find("粘贴") != std::string::npos) ||
	            (textContent.find("贴") != std::string::npos) ||
	            (textContent.find("文件内容") != std::string::npos);

	        const Json::Value& toolDefs =
	            (!session.tools_raw.isNull() && session.tools_raw.isArray() && session.tools_raw.size() > 0)
	                ? session.tools_raw
	                : session.tools;
	        auto hasTool = [&](const std::string& name) -> bool {
	            if (!toolDefs.isArray()) return false;
	            for (const auto& t : toolDefs) {
	                if (!t.isObject()) continue;
	                if (t.get("type", "").asString() != "function") continue;
	                const auto& func = t["function"];
	                if (!func.isObject()) continue;
	                if (func.get("name", "").asString() == name) return true;
	            }
	            return false;
	        };

		        auto trimPunct = [](std::string s) -> std::string {
		            auto isTrim = [](unsigned char c) {
		                return std::isspace(c) || c == '`' || c == '"' || c == '\'' ||
		                       c == ',' || c == ';' || c == ':' || c == '.' ||
		                       c == ')' || c == '(' || c == ']' || c == '[' ||
		                       c == '>' || c == '<' || c == '!' || c == '?';
		            };
		            while (!s.empty() && isTrim(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
		            while (!s.empty() && isTrim(static_cast<unsigned char>(s.back()))) s.pop_back();
		            return s;
		        };

	        auto looksLikeFilePath = [](const std::string& s) -> bool {
	            if (s.find('/') == std::string::npos) return false;
	            size_t lastSlash = s.find_last_of('/');
	            size_t lastDot = s.find_last_of('.');
	            if (lastDot == std::string::npos || lastDot < lastSlash) return false;
	            // keep it simple: ensure extension is short-ish
	            return (s.size() - lastDot) <= 8;
	        };

	        auto extractBacktickPaths = [&](const std::string& s) -> std::vector<std::string> {
	            std::vector<std::string> out;
	            size_t i = 0;
	            while (true) {
	                size_t a = s.find('`', i);
	                if (a == std::string::npos) break;
	                size_t b = s.find('`', a + 1);
	                if (b == std::string::npos) break;
	                std::string seg = trimPunct(s.substr(a + 1, b - a - 1));
	                if (looksLikeFilePath(seg)) out.push_back(std::move(seg));
	                i = b + 1;
	            }
	            return out;
	        };

	        if (looksLikeNoToolAccess && hasTool("read_file")) {
	            std::vector<std::string> paths = extractBacktickPaths(textContent);
	            // If no backtick paths, do a light-weight token scan.
	            if (paths.empty()) {
	                std::string token;
	                for (char ch : textContent) {
	                    const bool split = std::isspace(static_cast<unsigned char>(ch)) ||
	                                       ch == ',' || ch == ';' || ch == ':' || ch == '\n' ||
	                                       ch == '\r' || ch == '\t';
	                    if (!split) {
	                        token.push_back(ch);
	                        continue;
	                    }
	                    if (!token.empty()) {
	                        std::string cand = trimPunct(token);
	                        if (looksLikeFilePath(cand)) paths.push_back(std::move(cand));
	                        token.clear();
	                    }
	                }
	                if (!token.empty()) {
	                    std::string cand = trimPunct(token);
	                    if (looksLikeFilePath(cand)) paths.push_back(std::move(cand));
	                }
	            }

	            // Deduplicate + limit.
	            std::vector<std::string> uniq;
	            for (auto& p : paths) {
	                bool seen = false;
	                for (const auto& u : uniq) {
	                    if (u == p) {
	                        seen = true;
	                        break;
	                    }
	                }
	                if (!seen) uniq.push_back(std::move(p));
	                if (uniq.size() >= 10) break;
	            }

	            if (!uniq.empty()) {
	                Json::Value args(Json::objectValue);
	                Json::Value files(Json::arrayValue);
	                for (const auto& p : uniq) {
	                    Json::Value f(Json::objectValue);
	                    f["path"] = p;
	                    files.append(f);
	                }
	                args["files"] = files;

	                generation::ToolCallDone tc;
	                tc.id = generateFallbackToolCallId();
	                tc.name = "read_file";
	                tc.index = 0;

	                Json::StreamWriterBuilder writer;
	                writer["indentation"] = "";
	                tc.arguments = Json::writeString(writer, args);

	                toolCalls.push_back(tc);
	                textContent.clear();
	                LOG_WARN << "[GenerationService][" << clientType << "] 上游拒绝工具访问，已自动转换为 read_file 工具调用";
	            }
	        }
	    }

	    // Kilo-Code / RooCode compatibility fallback:
	    // These clients enforce "exactly one tool call per assistant response". If the upstream
	    // doesn't emit any tool call markers, we wrap the plain text into an `attempt_completion`
	    // tool call so the client doesn't raise "You did not use a tool" errors.
    //
    // Also, when a tool call exists, we suppress visible text output (keep only the
    // zero-width session id that we embed later) to avoid mixing normal content with tool calls.
    if (strictToolClient) {
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
            textContent.clear(); // keep only zero-width session id in OutputTextDone
        } else if (!toolCalls.empty()) {
            // Keep content empty (still embeds session id later)
            textContent.clear();
        }
    }

    // Strict clients require exactly one tool call per assistant response.
    if (strictToolClient && toolCalls.size() > 1) {
        LOG_WARN << "[GenerationService][" << clientType << "] 检测到多个 tool call，已仅保留第一个以满足客户端约束";
        toolCalls.erase(toolCalls.begin() + 1, toolCalls.end());
    }

    // Emit tool calls (after any validation / strict-client fallback) so we never stream
    // an invalid tool call and then "change our mind" later.
    for (const auto& tc : toolCalls) {
        sink.onEvent(tc);
    }
    
    // 3. 如果使用零宽字符模式，在响应末尾嵌入会话ID
    auto& sessionManager = *chatSession::getInstance();
    if (sessionManager.isZeroWidthMode() && !session.curConversationId.empty()) {
        textContent = chatSession::embedSessionIdInText(textContent, session.curConversationId);
        LOG_DEBUG << "[生成服务] 已在响应中嵌入会话ID: " << session.curConversationId;
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
        LOG_DEBUG << "[GenerationService] 通道支持 tool calls，跳过 Bridge 处理";
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
                    LOG_DEBUG << "[GenerationService] 工具调用结束: " << event.toolName;
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
        LOG_DEBUG << "[生成服务] 已在响应中嵌入会话ID: " << session.curConversationId;
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
        LOG_DEBUG << "[GenerationService] 通道 " << channelName
                  << " supportsToolCalls: " << channelInfo.supportsToolCalls;
        return channelInfo.supportsToolCalls;
    }
    
    // 默认返回 true（保守策略，避免破坏现有行为）
    LOG_WARN << "[GenerationService] 未找到通道 " << channelName << "，默认支持 tool calls";
    return true;
}

void GenerationService::transformRequestForToolBridge(session_st& session) {
    const std::string clientType = session.client_info.get("client_type", "").asString();
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
        oss << "API 定义（Schema）：\n";
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
                const auto requiredKeys = collectSchemaKeys(schema);
                const auto& props = schema["properties"];

                for (const auto& key : requiredKeys) {
                    if (!props.isObject() || !props.isMember(key) || !props[key].isObject()) {
                        params.push_back(key);
                        continue;
                    }
                    params.push_back(key + ":" + describeSchema(props[key]));
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

    // Low-friction "task" prompt:
    // Modern RLHF-aligned models may reject "system override" style tool instructions.
    // We present this as a formatting/extraction task and ask the model to output
    // a strictly delimited XML block (Toolify-style) when an action/tool call is needed.
	    {
	        std::ostringstream policy;
	        if (strictToolClient) {
	            policy << "场景：软件工程协作\n";
	            policy << "任务类型：下一步工具调用指令生成（工具由外部系统执行）\n";
	            policy << "目标：根据【用户请求】选择并输出下一步要调用的 1 个工具。\n";
	            policy << "注意：不要解释你是否有权限/访问能力，也不要要求用户粘贴文件内容；根据你的需要使用列出的工具(比如需要文件就直接调用 read_file/list_files)。\n";
	        } else {
	            policy << "场景：后端接口测试数据生成\n";
	            policy << "任务类型：文本解析与特定格式数据生成\n";
	            policy << "目标：将【用户输入】的输出转换为后端 API 调用所需的 XML 数据（仅生成调用数据）。\n";
	        }

        if (!forcedToolName.empty()) {
            policy << "本次任务指定调用的 API： " << forcedToolName << "。\n";
        } else if (toolChoice == "required") {
            policy << "本次任务必须输出 1 个工具调用（请选择最合适的一个 API）。\n";
	        } else if (strictToolClient) {
	            policy << "本次任务每次回复必须输出 1 个工具调用。\n";
	            policy << "若不需要调用其它工具，请使用 attempt_completion 输出最终结果。\n";
	        } else {
	            policy << "当且仅当需要调用 API 才能完成任务时，才输出所需工具的 XML 格式；否则正常回答。\n";
	        }

        policy << "要求：\n";
        policy << "- 当输出工具调用时，除 XML 外不要输出任何其它文本（不要解释、不要前后缀、不要 markdown 代码块）。\n";
        policy << "- 必须严格按下面的 XML 格式输出（第1行是触发标记，第2行开始紧跟 <function_calls>）：\n";
        policy << triggerSignal << "\n";
        policy << "<function_calls>\n";
        policy << "  <function_call>\n";
        policy << "    <tool>TOOL_NAME</tool>\n";
        policy << "    <args_json><![CDATA[{\"PARAM_NAME\":\"VALUE\"}]]></args_json>\n";
        policy << "  </function_call>\n";
        policy << "</function_calls>\n";
        policy << "- <tool> 必须是 API 定义里存在的名称。\n";
        policy << "- <args_json> 必须是严格 JSON 对象，参数 key 必须与 API 定义完全一致（区分大小写）。\n";

        if (strictToolClient) {
            policy << "（" << clientType << "）每次回复必须且只能输出 1 个 <function_call>。\n";
            policy << "（" << clientType << "）当需要给出最终答复时，请输出工具调用：\n";
            policy << triggerSignal << "\n";
            policy << "<function_calls>\n";
            policy << "  <function_call>\n";
            policy << "    <tool>attempt_completion</tool>\n";
            policy << "    <args_json><![CDATA[{\"result\":\"...\"}]]></args_json>\n";
            policy << "  </function_call>\n";
            policy << "</function_calls>\n";
        }

        policy << "\n";
        toolDefinitions = policy.str() + toolDefinitions;
    }

    LOG_INFO << "[GenerationService] 注入工具定义到 requestmessage, 长度: " << toolDefinitions.length();

    std::string originalInput = session.requestmessage;
    LOG_DEBUG << "工具定义:" << toolDefinitions;
    session.requestmessage = toolDefinitions + "\n\n" + originalInput;

    // 清除 tools，避免后续流程再次处理
    session.tools = Json::Value(Json::nullValue);
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
        std::string role = msg.get("role", "user").asString();
        std::string content = msg.get("content", "").asString();
        
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
