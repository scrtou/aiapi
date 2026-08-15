#ifndef REQUEST_ADAPTERS_H
#define REQUEST_ADAPTERS_H

#include <application/generation/contracts/GenerationRequest.h>
#include <application/generation/contracts/GenerationSession.h>
#include <json/json.h>
#include <domain/port/IAccountSettingsQuery.h>
#include <domain/model/AiApiData.h>

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
    static void setTrackingMode(SessionTrackingMode mode) { trackingMode_ = mode; }
    static void setAccountSettingsQuery(IAccountSettingsQuery* query)
    { accountSettings_ = query; }
    /// Normalize a Chat Completions payload and copied HTTP metadata.
    static GenerationRequest buildGenerationRequestFromChat(
        const Json::Value& requestBody,
        const aiapi::RequestHeaders& headers
    );
    
    /// Normalize a Responses payload and copied HTTP metadata.
    static GenerationRequest buildGenerationRequestFromResponses(
        const Json::Value& requestBody,
        const aiapi::RequestHeaders& headers
    );
    
private:
    static SessionTrackingMode trackingMode_;
    static IAccountSettingsQuery* accountSettings_;
    /**
     * @brief 从 HTTP 请求中提取客户端信息
     * 
     * @param req HTTP 请求
     * @return Json::Value 客户端信息
     */
    static Json::Value extractClientInfo(const aiapi::RequestHeaders& headers);
    
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
     */
    static void parseChatMessages(
        const Json::Value& messages,
        std::vector<Message>& result,
        std::string& systemPrompt,
        std::string& currentInput,
        std::vector<ImageInfo>& images
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
     * @param[in,out] systemPrompt 提取的系统/开发者指令
     * @param[out] currentInput 当前轮用户输入及工具结果（供 XML bridge 使用）
     * @param[out] images 提取的图片信息
     */
    static void parseResponseInput(
        const Json::Value& input,
        std::vector<Message>& messages,
        std::string& systemPrompt,
        std::string& currentInput,
        std::vector<ImageInfo>& images
    );

    /**
     * @brief 解析 Responses API 的 input_items 字段（部分客户端使用）
     *
     * 目前仅将其中的文本汇总到 currentInput，并解析图片。
     */
    static void parseResponseInputItems(
        const Json::Value& inputItems,
        std::string& currentInput,
        std::vector<ImageInfo>& images
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
        bool stripZeroWidth = false,
        std::vector<std::string>* outRawTexts = nullptr
    );
    
    /**
     * @brief 解析图片 URL 为 ImageInfo
     * 
     * @param url 图片 URL（可以是 data: URL 或普通 URL）
     * @return ImageInfo 图片信息
     */
    static ImageInfo parseImageUrl(const std::string& url);
};

#endif // 头文件保护结束
