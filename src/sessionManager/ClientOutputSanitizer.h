#ifndef CLIENT_OUTPUT_SANITIZER_H
#define CLIENT_OUTPUT_SANITIZER_H

#include <string>
#include <vector>
#include <json/json.h>

/**
 * @brief 客户端输出清洗器
 * 
 * 将 Kilo/Roo Code 等客户端的工具标签清洗逻辑从 Controller 层抽离到 Session 层。
 * 这是"业务输出规则"，与协议无关（同一文本应该在 Chat/Responses 上表现一致）。
 * 
 * 参考设计文档: plans/aiapi-refactor-design.md 第 5.3 节
 */
class ClientOutputSanitizer {
public:
    /**
     * @brief 清洗输出文本
     * 
     * 根据客户端类型对输出文本进行清洗和纠正：
     * 1. 基础标签纠错（修正模型常见的拼写错误）
     * 2. 检测 attempt_completion 包裹其他工具调用的情况
     * 3. 兼容性提取逻辑（处理 markdown 包裹 ```xml ... ```）
     * 
     * @param clientInfo 客户端信息 JSON（包含 client_type 字段）
     * @param text 原始输出文本
     * @return 清洗后的文本
     */
    static std::string sanitize(const Json::Value& clientInfo, const std::string& text);
    
    /**
     * @brief 检查是否需要清洗
     * 
     * @param clientInfo 客户端信息 JSON
     * @return true 如果需要清洗
     */
    static bool needsSanitize(const Json::Value& clientInfo);
    
private:
    /**
     * @brief 替换所有匹配的字符串
     */
    static void replaceAll(std::string& str, const std::string& from, const std::string& to);
    
    /**
     * @brief Kilo/Roo Code 支持的工具列表
     */
    static const std::vector<std::string>& getKiloTools();
    
    /**
     * @brief 修正常见的标签拼写错误
     */
    static void fixCommonTagErrors(std::string& message);
    
    /**
     * @brief 检测并修复 attempt_completion 错误包裹其他工具的情况
     * @return true 如果进行了修复
     */
    static bool fixAttemptCompletionWrapping(std::string& message);
    
    /**
     * @brief 处理 markdown 包裹的工具标签
     */
    static void fixMarkdownWrapping(std::string& message);
};

#endif // CLIENT_OUTPUT_SANITIZER_H
