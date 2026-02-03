#ifndef GENERATION_REQUEST_H
#define GENERATION_REQUEST_H

#include <string>
#include <vector>
#include <optional>
#include <json/json.h>

/**
 * @brief 请求入口类型（API endpoint）
 *
 * 用于区分 Chat Completions 与 Responses 两类入口。
 */
enum class EndpointType {
    ChatCompletions,   // OpenAI Chat Completions API (/v1/chat/completions)
    Responses          // OpenAI Responses API (/v1/responses)
};

/**
 * @brief 消息角色
 */
enum class MessageRole {
    System,
    User,
    Assistant,
    Tool
};

/**
 * @brief 内容部分类型
 */
enum class ContentPartType {
    Text,
    Image
};

/**
 * @brief 内容部分
 * 
 * 消息可以包含多个内容部分（文本、图片等）
 */
struct ContentPart {
    ContentPartType type = ContentPartType::Text;
    std::string text;          // 文本内容
    std::string imageUrl;      // 图片URL或base64
    std::string mediaType;     // 图片媒体类型 (image/png, image/jpeg等)
};

/**
 * @brief 消息结构
 *
 * 内部强类型消息表示
 */
struct Message {
    MessageRole role = MessageRole::User;
    std::vector<ContentPart> content;
    
    // Tool call 相关字段
    std::vector<Json::Value> toolCalls;  // assistant 消息中的 tool calls
    std::string toolCallId;               // tool 消息对应的 tool call ID
    
    // 便捷构造函数
    static Message user(const std::string& text) {
        Message msg;
        msg.role = MessageRole::User;
        ContentPart part;
        part.type = ContentPartType::Text;
        part.text = text;
        msg.content.push_back(part);
        return msg;
    }
    
    static Message assistant(const std::string& text) {
        Message msg;
        msg.role = MessageRole::Assistant;
        ContentPart part;
        part.type = ContentPartType::Text;
        part.text = text;
        msg.content.push_back(part);
        return msg;
    }
    
    static Message system(const std::string& text) {
        Message msg;
        msg.role = MessageRole::System;
        ContentPart part;
        part.type = ContentPartType::Text;
        part.text = text;
        msg.content.push_back(part);
        return msg;
    }
    
    // 获取纯文本内容
    std::string getTextContent() const {
        std::string result;
        for (const auto& part : content) {
            if (part.type == ContentPartType::Text) {
                result += part.text;
            }
        }
        return result;
    }
};

/**
 * @brief 图片信息
 *
 * 当前请求中的图片列表
 */
struct ImageInfo {
    std::string base64Data;      // base64编码的图片数据
    std::string mediaType;       // 图片类型如 image/png, image/jpeg
    std::string uploadedUrl;     // 上传后的图片URL
    int width = 0;
    int height = 0;
};

/**
 * @brief 统一生成请求
 *
 * 统一"业务语义"，而不是强行对齐 HTTP 字段。
 * Chat Completions 和 Responses API 都映射到这个结构。
 *
 * 参考设计文档: plans/aiapi-refactor-design.md 第 4.1 节
 */
struct GenerationRequest {
    // ========== 入口/会话连续性 ==========
    EndpointType endpointType = EndpointType::ChatCompletions;
    std::optional<std::string> previousResponseId;   // /v1/responses: previous_response_id（可选）
    std::vector<std::string> continuityTexts;        // 原始文本集合（用于 ZeroWidth / 其它续聊解析，保留零宽字符）
    Json::Value clientInfo;                          // 后续可换成强类型
    
    // ========== 生成目标 ==========
    std::string provider;           // 如 "chaynsapi"
    std::string model;              // 模型名称
    std::string systemPrompt;       // 系统提示词
    
    // ========== 输入 ==========
    std::vector<Message> messages;  // 内部强类型消息列表
    std::string currentInput;       // 当前用户输入（纯文本）
    std::vector<ImageInfo> images;  // 当前请求中的图片列表
    
    // ========== 工具调用 ==========
    Json::Value tools;               // 工具定义列表
    std::string toolChoice;          // 工具选择策略 (auto/none/required)
    
    // ========== 输出要求 ==========
    bool stream = false;            // 是否流式输出
    
    // ========== 追踪 ==========
    std::string requestId;          // 请求 ID（可选）
    std::string traceId;            // 追踪 ID（可选）
    
    // ========== 辅助方法 ==========
    
    /**
     * @brief 检查是否为 Response API 请求
     */
    bool isResponseApi() const {
        return endpointType == EndpointType::Responses;
    }
    
    /**
     * @brief 获取所有消息的纯文本内容（用于日志等）
     */
    std::string getMessagesText() const {
        std::string result;
        for (const auto& msg : messages) {
            result += msg.getTextContent() + "\n";
        }
        return result;
    }
};

#endif // GENERATION_REQUEST_H
