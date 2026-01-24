#ifndef REQUEST_ADAPTERS_H
#define REQUEST_ADAPTERS_H

#include "GenerationRequest.h"
#include "Session.h"
#include <drogon/HttpRequest.h>
#include <json/json.h>

/**
 * @brief 请求适配器
 * 
 * 负责将 HTTP 请求转换为统一的 GenerationRequest。
 * 这是 "HTTP → GenerationRequest" 转换的唯一实现点。
 * 
 * 参考设计文档: plans/single-entrypoint-generationrequest-plan.md PR1
 */
class RequestAdapters {
public:
    /**
     * @brief 从 Chat Completions API 请求构建 GenerationRequest
     * 
     * 解析 OpenAI Chat Completions API 格式的请求，提取：
     * - model: 模型名称
     * - messages: 消息数组（包含 system/user/assistant 角色）
     * - stream: 是否流式输出
     * - 客户端信息（从 HTTP 头提取）
     * 
     * @param req HTTP 请求
     * @return GenerationRequest 统一生成请求
     */
    static GenerationRequest buildGenerationRequestFromChat(
        const drogon::HttpRequestPtr& req
    );
    
    /**
     * @brief 从 Responses API 请求构建 GenerationRequest
     * 
     * 解析 OpenAI Responses API 格式的请求，提取：
     * - model: 模型名称
     * - input: 输入内容（字符串或数组）
     * - instructions: 系统指令（映射到 systemPrompt）
     * - previous_response_id: 前一个响应ID（用于续聊）
     * - stream: 是否流式输出
     * - 客户端信息（从 HTTP 头提取）
     * 
     * @param req HTTP 请求
     * @return GenerationRequest 统一生成请求
     */
    static GenerationRequest buildGenerationRequestFromResponses(
        const drogon::HttpRequestPtr& req
    );
    
private:
    /**
     * @brief 从 HTTP 请求中提取客户端信息
     * 
     * @param req HTTP 请求
     * @return Json::Value 客户端信息
     */
    static Json::Value extractClientInfo(const drogon::HttpRequestPtr& req);
    
    /**
     * @brief 解析 Chat API 的 messages 数组
     *
     * 将 JSON 格式的 messages 数组转换为内部 Message 结构。
     * 支持：
     * - 简单字符串 content
     * - 数组格式 content（包含 text 和 image_url 类型）
     *
     * @param messages JSON messages 数组
     * @param[out] result 解析后的消息列表
     * @param[out] systemPrompt 提取的系统提示词
     * @param[out] currentInput 当前用户输入
     * @param[out] images 提取的图片信息
     * @param[out] extractedSessionId 从零宽字符中提取的会话ID（如果有）
     */
    static void parseChatMessages(
        const Json::Value& messages,
        std::vector<Message>& result,
        std::string& systemPrompt,
        std::string& currentInput,
        std::vector<ImageInfo>& images,
        std::string& extractedSessionId
    );
    
    /**
     * @brief 解析 Responses API 的 input 字段
     * 
     * 支持：
     * - 简单字符串输入
     * - 数组格式输入（包含历史消息和当前输入）
     * 
     * @param input JSON input 字段
     * @param[out] messages 解析后的历史消息
     * @param[out] currentInput 当前用户输入
     * @param[out] images 提取的图片信息
     * @param[out] extractedSessionId 从零宽字符中提取的会话ID（如果有）
     */
    static void parseResponseInput(
        const Json::Value& input,
        std::vector<Message>& messages,
        std::string& currentInput,
        std::vector<ImageInfo>& images,
        std::string& extractedSessionId
    );
    
    /**
     * @brief 从 content 字段中提取文本和图片
     * 
     * @param content JSON content 字段（字符串或数组）
     * @param[out] images 提取的图片信息
     * @param stripZeroWidth 是否移除零宽字符
     * @return std::string 提取的文本内容
     */
    static std::string extractContentText(
        const Json::Value& content,
        std::vector<ImageInfo>& images,
        bool stripZeroWidth = false
    );
    
    /**
     * @brief 解析图片 URL 为 ImageInfo
     * 
     * @param url 图片 URL（可以是 data: URL 或普通 URL）
     * @return ImageInfo 图片信息
     */
    static ImageInfo parseImageUrl(const std::string& url);
};

#endif // REQUEST_ADAPTERS_H
