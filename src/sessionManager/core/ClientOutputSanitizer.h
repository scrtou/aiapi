#ifndef CLIENT_OUTPUT_SANITIZER_H
#define CLIENT_OUTPUT_SANITIZER_H

#include <string>
#include <vector>
#include <json/json.h>

/**
 * @brief 客户端输出清洗器
 * 
 * 职责已收敛为纯文本清洗:
 * - 修正模型常见的标签拼写错误
 * - 去除非法控制字符
 * - 基础格式修复
 * 
 * 注意: 协议转换逻辑已迁移到 ToolCallBridge
 * 参考设计文档: plans/tool_call_bridge_design.md Phase 5
 */
class ClientOutputSanitizer {
public:
    /**
     * @brief 清洗输出文本
     * 
     * 根据客户端类型对输出文本进行清洗和纠正:
     * 1. 基础标签纠错（修正模型常见的拼写错误）
     * 2. 去除非法控制字符
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
    
    /**
     * @brief 去除非法控制字符
     * 
     * 移除不可打印的控制字符（保留换行、制表符等常用字符）
     * 
     * @param text 原始文本
     * @return 清洗后的文本
     */
    static std::string removeControlCharacters(const std::string& text);
    
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
};

#endif // 头文件保护结束
