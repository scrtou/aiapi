#pragma once

#include <json/json.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include "sessionManager/contracts/GenerationEvent.h"
namespace toolcall {

/**
 * @brief 工具调用校验模式
 *
 * 不同客户端可能需要不同级别的校验严格程度：
 * - None: 不校验，完全信任 AI 输出（类似 Toolify 的策略）
 * - Relaxed: 只校验关键字段（path, content 等）
 * - Strict: 完整 schema 校验（所有 required 字段 + 类型检查）
 */
enum class ValidationMode {
    None,      // 不校验 - 信任 AI 输出，只解析 JSON
    Relaxed,   // 宽松校验 - 只校验关键字段（Roo/Kilo 客户端默认使用）
    Strict     // 严格校验 - 完整校验（所有必填字段 + 类型检查）
};

/**
 * @brief 工具调用校验结果
 */
struct ValidationResult {
    bool valid = false;
    std::string errorMessage;
    
    static ValidationResult success() {
        return {true, ""};
    }
    
    static ValidationResult failure(const std::string& msg) {
        return {false, msg};
    }
};

/**
 * @brief 工具调用校验器 - 根据客户端提供的 schema 校验工具调用
 *
 * 支持三种校验模式：
 * - None: 不校验，只解析 JSON（信任 AI 输出）
 * - Relaxed: 只校验关键字段（path/content 等必须存在且非空）
 * - Strict: 完整 schema 校验（所有 required 字段 + 类型检查）
 *
 * 支持按客户端类型使用不同的关键字段集合：
 * - RooCode/Kilo-Code: 完整的关键字段集合
 * - 其他客户端: 最小关键字段集合
 */
class ToolCallValidator {
public:
    /**
     * @brief 构造校验器
     * @param toolDefs 客户端请求中的工具定义数组（校验的 source of truth）
     * @param clientType 客户端类型（用于选择关键字段集合，默认为空）
     */
    explicit ToolCallValidator(const Json::Value& toolDefs, const std::string& clientType = "");
    
    /**
     * @brief 校验单个工具调用
     * @param toolCall 要校验的工具调用
     * @param mode 校验模式（默认: None - 不校验）
     * @return ValidationResult 校验结果，包含是否通过和错误信息
     */
    ValidationResult validate(
        const generation::ToolCallDone& toolCall,
        ValidationMode mode = ValidationMode::None
    ) const;
    
    /**
     * @brief 过滤工具调用，移除无效的调用
     * @param toolCalls 工具调用列表（会被修改）
     * @param discardedText 输出：被丢弃的工具调用信息（用于降级策略）
     * @param mode 校验模式（默认: None - 不校验）
     * @return 被移除的工具调用数量
     */
    size_t filterInvalidToolCalls(
        std::vector<generation::ToolCallDone>& toolCalls,
        std::string& discardedText,
        ValidationMode mode = ValidationMode::None
    ) const;
    
    /**
     * @brief 检查工具名是否存在于工具定义中
     * @param toolName 工具名
     * @return 如果工具存在返回 true
     */
    bool hasToolDefinition(const std::string& toolName) const;
    
    /**
     * @brief 获取有效工具名列表
     * @return 有效工具名集合
     */
    const std::unordered_set<std::string>& getValidToolNames() const;
    
    /**
     * @brief 获取当前客户端类型
     * @return 客户端类型字符串
     */
    const std::string& getClientType() const { return clientType_; }

private:
    /**
     * @brief 根据工具名查找工具定义
     * @param toolName 工具名
     * @return 工具定义指针，如果未找到返回 nullptr
     */
    const Json::Value* findToolDefinition(const std::string& toolName) const;
    
    /**
     * @brief 校验参数 JSON 是否可解析
     * @param arguments 参数字符串
     * @param parsedArgs 输出：解析后的 JSON 值
     * @return ValidationResult
     */
    ValidationResult validateArgumentsParseable(
        const std::string& arguments,
        Json::Value& parsedArgs
    ) const;
    
    /**
     * @brief 校验必需字段是否存在
     * @param toolName 工具名（用于错误信息）
     * @param args 解析后的参数
     * @param schema 参数 schema
     * @return ValidationResult
     */
    ValidationResult validateRequiredFields(
        const std::string& toolName,
        const Json::Value& args,
        const Json::Value& schema
    ) const;
    
    /**
     * @brief 校验字段类型是否匹配 schema
     * @param toolName 工具名（用于错误信息）
     * @param args 解析后的参数
     * @param schema 参数 schema
     * @return ValidationResult
     */
    ValidationResult validateFieldTypes(
        const std::string& toolName,
        const Json::Value& args,
        const Json::Value& schema
    ) const;
    
    /**
     * @brief 校验关键字符串字段是否非空
     * 这是对 path, diff, content 等字段的安全检查，
     * 这些字段为空时没有意义。
     * @param toolName 工具名
     * @param args 解析后的参数
     * @return ValidationResult
     */
    ValidationResult validateCriticalFieldsNonEmpty(
        const std::string& toolName,
        const Json::Value& args
    ) const;
    
    /**
     * @brief 检查字段是否为关键字段（必须非空）
     * @param toolName 工具名
     * @param fieldName 字段名
     * @return 如果是关键字段返回 true
     */
    bool isCriticalField(const std::string& toolName, const std::string& fieldName) const;
    
    /**
     * @brief 初始化客户端特定的关键字段集合
     */
    void initCriticalFieldsForClient();

private:
    Json::Value toolDefs_;                              // 工具定义
    std::unordered_set<std::string> validToolNames_;    // 有效工具名集合
    std::string clientType_;                            // 客户端类型
    std::unordered_set<std::string> criticalFields_;    // 当前客户端的关键字段集合
    
    // 不同客户端的关键字段定义
    // RooCode/Kilo-Code： 完整集合（这些客户端对工具调用格式要求严格）
    // 默认: 最小集合（只包含最基本的字段）
    static const std::unordered_set<std::string> kRooKiloCriticalFields;
    static const std::unordered_set<std::string> kDefaultCriticalFields;
};

/**
 * @brief 校验失败后的降级策略
 */
enum class FallbackStrategy {
    DiscardOnly,           // 仅丢弃无效的工具调用（非严格客户端）
    WrapAttemptCompletion, // 将文本包装为 attempt_completion（严格客户端如 Roo/Kilo）
    GenerateReadFile       // 生成 read_file 降级调用（严格客户端）
};

/**
 * @brief 应用校验失败后的降级策略
 * @param clientType 客户端类型（如 "RooCode", "Kilo-Code"）
 * @param toolCalls 工具调用列表（可能被修改）
 * @param textContent 文本内容（可能被修改）
 * @param discardedText 被丢弃的工具调用信息
 * @return 应用的降级策略
 */
FallbackStrategy applyValidationFallback(
    const std::string& clientType,
    std::vector<generation::ToolCallDone>& toolCalls,
    std::string& textContent,
    const std::string& discardedText
);

/**
 * @brief 检查客户端是否需要严格的工具调用处理
 * @param clientType 客户端类型
 * @return 如果是严格客户端（Roo/Kilo）返回 true
 */
bool isStrictToolClient(const std::string& clientType);

/**
 * @brief 根据客户端类型获取推荐的校验模式
 * @param clientType 客户端类型
 * @return 推荐的校验模式
 */
ValidationMode getRecommendedValidationMode(const std::string& clientType);

} // 命名空间结束
