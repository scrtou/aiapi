#include "GenerationService.h"
#include "ClientOutputSanitizer.h"
#include <apiManager/ApiManager.h>
#include <apipoint/ProviderResult.h>
#include <tools/ZeroWidthEncoder.h>
#include <drogon/drogon.h>

using namespace drogon;
using namespace provider;
using namespace session;
using namespace error;

// ========== 共享 helper 实现 ==========

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
    session.last_active_time = time(nullptr);
    session.created_time = time(nullptr);
    
    // 协议相关
    session.is_response_api = req.isResponseApi();
    
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
        std::string responseId = sessionManager.createResponseSession(session);
        LOG_INFO << "[生成服务] 创建 Response 会话, response_id: " << responseId;
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
    
    // 应用输出清洗
    text = sanitizeOutput(session.client_info, text);
    
    // 如果使用零宽字符模式，在响应末尾嵌入会话ID
    auto& sessionManager = *chatSession::getInstance();
    if (sessionManager.isZeroWidthMode() && !session.curConversationId.empty()) {
        text = chatSession::embedSessionIdInText(text, session.curConversationId);
        LOG_DEBUG << "[生成服务] 已在响应中嵌入会话ID: " << session.curConversationId;
    }
    
    // 发送 OutputTextDone 事件
    generation::OutputTextDone textDone;
    textDone.text = text;
    textDone.index = 0;
    sink.onEvent(textDone);
    
    // 发送 Completed 事件
    generation::Completed completed;
    completed.finishReason = "stop";
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
