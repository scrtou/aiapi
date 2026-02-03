#ifndef GENERATION_SERVICE_H
#define GENERATION_SERVICE_H

#include "GenerationRequest.h"
#include "GenerationEvent.h"
#include "IResponseSink.h"
#include "Session.h"
#include "SessionExecutionGate.h"
#include "Errors.h"
#include "ToolCallBridge.h"
#include "XmlTagToolCallCodec.h"

/**
 * @brief 生成服务
 *
 * 统一的业务编排层，负责：
 * 1. 接收 GenerationRequest + IResponseSink
 * 2. 通过 SessionStore 获取/更新会话上下文
 * 3. 调用 provider 执行一次生成
 * 4. 将结果（或 provider 事件）转换成 GenerationEvent 发送给 sink
 * 5. 统一错误捕获、映射与清理
 *
 * 参考设计文档: plans/aiapi-refactor-design.md 第 5.1 节
 */
class GenerationService {
public:
    GenerationService() = default;
    ~GenerationService() = default;
    
    // 禁止拷贝和移动
    GenerationService(const GenerationService&) = delete;
    GenerationService& operator=(const GenerationService&) = delete;
    GenerationService(GenerationService&&) = delete;
    GenerationService& operator=(GenerationService&&) = delete;
    
    // ========== 新主入口（接收 GenerationRequest）==========
    
    /**
     * @brief 【新主入口】从 GenerationRequest 执行生成（带执行门控）
     *
     * Controller 统一只调用此方法。职责：
     * 1. materialize: GenerationRequest → session_st
     * 2. create/update session（必须发生在门控前）
     * 3. 调用共享 helper executeGuardedWithSession()
     *
     * @param req 生成请求（由 RequestAdapters 构建）
     * @param sink 输出通道
     * @param policy 并发策略
     * @return 应用层错误（如果有）
     */
    std::optional<error::AppError> runGuarded(
        const GenerationRequest& req,
        IResponseSink& sink,
        session::ConcurrencyPolicy policy = session::ConcurrencyPolicy::RejectConcurrent
    );
    
private:
    // ========== 共享 helper（新旧入口复用）==========
    
    /**
     * @brief 计算执行门控的 key
     *
     * 门控 key 统一使用 sessionId（即 session.curConversationId）。
     * Responses API 的 response.id 为每次请求生成，不参与门控（避免同一会话并发打架）。
     *
     * @param session 会话状态
     * @return 门控 key，空字符串表示不使用门控
     */
    static std::string computeExecutionKey(const session_st& session);
    
    /**
     * @brief 【核心 helper】带门控执行生成
     *
     * 核心执行逻辑，负责：
     * - guard key 计算入口（调用 computeExecutionKey）
     * - ExecutionGuard 生命周期（构造/获取/失败分支处理）
     * - 取消检查：guard.isCancelled() 前后各一次
     * - guard 内执行包裹：Started/Provider调用/emitResultEvents/会话写回/afterResponseProcess
     * - 错误/关闭语义
     *
     * @param session 已初始化的会话状态（会被修改）
     * @param sink 输出通道
     * @param stream 是否流式输出
     * @param policy 并发策略
     * @return 应用层错误（如果有）
     */
    std::optional<error::AppError> executeGuardedWithSession(
        session_st& session,
        IResponseSink& sink,
        bool stream,
        session::ConcurrencyPolicy policy
    );
    
    /**
     * @brief 将 GenerationRequest 物化为 session_st
     *
     * 唯一实现点：GenerationRequest → session_st 的字段映射
     *
     * @param req 生成请求
     * @return 初始化的 session_st
     */
    static session_st materializeSession(const GenerationRequest& req);
    
    /**
     * @brief 调用 Provider 执行生成
     *
     * @param session 会话状态（包含输入和输出）
     * @return 是否成功
     */
    bool executeProvider(session_st& session);
    
    /**
     * @brief 将 Provider 结果转换为事件并发送
     *
     * @param session 会话状态
     * @param sink 输出通道
     */
    void emitResultEvents(const session_st& session, IResponseSink& sink);
    
    /**
     * @brief 发送错误事件
     *
     * @param code 错误代码
     * @param message 错误信息
     * @param sink 输出通道
     */
    void emitError(
        generation::ErrorCode code,
        const std::string& message,
        IResponseSink& sink
    );
    
    /**
     * @brief 应用输出清洗
     *
     * 使用 ClientOutputSanitizer 清洗输出文本
     *
     * @param clientInfo 客户端信息
     * @param text 原始文本
     * @return 清洗后的文本
     */
    std::string sanitizeOutput(const Json::Value& clientInfo, const std::string& text);
    
    /**
     * @brief 获取通道的 tool call 支持能力
     *
     * @param channelName 通道名称
     * @return 是否支持 tool calls
     */
    static bool getChannelSupportsToolCalls(const std::string& channelName);
    
    /**
     * @brief 为 ToolCallBridge 转换请求
     *
     * 当通道不支持 tool calls 时，将工具定义注入到 currentInput 前面
     * 使用 XmlTagToolCallCodec::encodeToolDefinitions() 生成文本格式的工具定义
     *
     * @param session 会话状态（会被修改）
     */
    static void transformRequestForToolBridge(session_st& session);
    
    // ========== emitResultEvents 辅助函数 ==========
     
     /**
      * @brief 解析 XML 格式的 tool calls（Toolify-style）
      *
      * @param xmlInput XML 输入文本
      * @param outTextContent 输出的纯文本内容
      * @param outToolCalls 输出的 tool calls 列表
      */
     static void parseXmlToolCalls(
         const std::string& xmlInput,
         std::string& outTextContent,
         std::vector<generation::ToolCallDone>& outToolCalls,
         const std::string& sentinel
     );
    
    /**
     * @brief 生成 tool_choice=required 时的兜底 tool call
     *
     * @param session 会话状态
     * @param outToolCalls 输出的 tool calls 列表
     * @param outTextContent 输出的文本内容（会被清空）
     */
    static void generateForcedToolCall(
        const session_st& session,
        std::vector<generation::ToolCallDone>& outToolCalls,
        std::string& outTextContent
    );
    
    /**
     * @brief 规范化 tool call 参数形状
     *
     * 根据客户端提供的 JSONSchema 规范化参数
     *
     * @param session 会话状态
     * @param toolCalls tool calls 列表（会被修改）
     */
    static void normalizeToolCallArguments(
        const session_st& session,
        std::vector<generation::ToolCallDone>& toolCalls
    );
    
    /**
     * @brief 应用严格客户端规则
     *
     * 包括：包装为 attempt_completion、限制单个 tool call
     *
     * @param clientType 客户端类型
     * @param textContent 文本内容（会被修改）
     * @param toolCalls tool calls 列表（会被修改）
     */
    static void applyStrictClientRules(
        const std::string& clientType,
        std::string& textContent,
        std::vector<generation::ToolCallDone>& toolCalls
    );
};

#endif // GENERATION_SERVICE_H
