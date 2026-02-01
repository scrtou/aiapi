#include "ToolCallValidator.h"
#include <drogon/drogon.h>
#include <sstream>
#include <random>
#include <iomanip>

namespace toolcall {

// ============================================================================
// 静态成员定义：不同客户端的关键字段集合
// ============================================================================

// RooCode/Kilo-Code 客户端的关键字段（完整集合）
// 这些客户端对工具调用格式要求严格，需要校验更多字段
const std::unordered_set<std::string> ToolCallValidator::kRooKiloCriticalFields = {
    "path",      // read_file, write_to_file, apply_diff, list_files, search_files
    "diff",      // apply_diff
    "content",   // write_to_file
    "command",   // execute_command
    "regex",     // search_files
    "question",  // ask_followup_question
    "result"     // attempt_completion
};

// 默认客户端的关键字段（最小集合）
// 只包含最基本的字段，避免过度校验
const std::unordered_set<std::string> ToolCallValidator::kDefaultCriticalFields = {
    "path",      // 文件操作的基本字段
    "content"    // 写入操作的基本字段
};

// ============================================================================
// 辅助函数
// ============================================================================

// 生成降级工具调用的 ID
static std::string generateFallbackToolCallId() {
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

// ============================================================================
// ToolCallValidator 实现
// ============================================================================

ToolCallValidator::ToolCallValidator(const Json::Value& toolDefs, const std::string& clientType)
    : toolDefs_(toolDefs)
    , clientType_(clientType)
{
    // 从工具定义中构建有效工具名集合
    if (toolDefs_.isArray()) {
        for (const auto& tool : toolDefs_) {
            if (!tool.isObject()) continue;
            if (tool.get("type", "").asString() != "function") continue;
            
            const auto& func = tool["function"];
            if (!func.isObject()) continue;
            
            std::string name = func.get("name", "").asString();
            if (!name.empty()) {
                validToolNames_.insert(name);
            }
        }
    }
    
    // 根据客户端类型初始化关键字段集合
    initCriticalFieldsForClient();
    
    LOG_INFO << "[ToolCallValidator] 初始化完成: "
              << "工具数量=" << validToolNames_.size()
              << ", 客户端类型=" << (clientType_.empty() ? "default" : clientType_)
              << ", 关键字段数量=" << criticalFields_.size();
}

void ToolCallValidator::initCriticalFieldsForClient() {
    // 根据客户端类型选择关键字段集合
    if (clientType_ == "RooCode" || clientType_ == "Kilo-Code") {
        // Roo/Kilo 客户端使用完整的关键字段集合
        criticalFields_ = kRooKiloCriticalFields;
    } else {
        // 其他客户端使用最小关键字段集合
        criticalFields_ = kDefaultCriticalFields;
    }
}

bool ToolCallValidator::hasToolDefinition(const std::string& toolName) const {
    return validToolNames_.find(toolName) != validToolNames_.end();
}

const std::unordered_set<std::string>& ToolCallValidator::getValidToolNames() const {
    return validToolNames_;
}

const Json::Value* ToolCallValidator::findToolDefinition(const std::string& toolName) const {
    if (!toolDefs_.isArray()) return nullptr;
    
    for (const auto& tool : toolDefs_) {
        if (!tool.isObject()) continue;
        if (tool.get("type", "").asString() != "function") continue;
        
        const auto& func = tool["function"];
        if (!func.isObject()) continue;
        
        if (func.get("name", "").asString() == toolName) {
            return &tool;
        }
    }
    
    return nullptr;
}

ValidationResult ToolCallValidator::validateArgumentsParseable(
    const std::string& arguments,
    Json::Value& parsedArgs
) const {
    if (arguments.empty()) {
        // 空参数是有效的（某些工具可能没有必需参数）
        parsedArgs = Json::Value(Json::objectValue);
        return ValidationResult::success();
    }
    
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream iss(arguments);
    
    if (!Json::parseFromStream(builder, iss, &parsedArgs, &errors)) {
        return ValidationResult::failure("参数 JSON 解析错误: " + errors);
    }
    
    if (!parsedArgs.isObject()) {
        return ValidationResult::failure("参数必须是 JSON 对象");
    }
    
    return ValidationResult::success();
}

ValidationResult ToolCallValidator::validateRequiredFields(
    const std::string& toolName,
    const Json::Value& args,
    const Json::Value& schema
) const {
    if (!schema.isObject()) {
        LOG_INFO << "[ToolCallValidator] validateRequiredFields: schema 不是对象，跳过校验";
        return ValidationResult::success();
    }
    
    const auto& required = schema["required"];
    if (!required.isArray()) {
        LOG_INFO << "[ToolCallValidator] validateRequiredFields: required 不是数组，跳过校验";
        return ValidationResult::success();
    }
    
    // 调试日志：打印 args 中的所有字段
    {
        std::ostringstream oss;
        oss << "[ToolCallValidator] validateRequiredFields: 工具=" << toolName << ", args字段=[";
        bool first = true;
        for (const auto& name : args.getMemberNames()) {
            if (!first) oss << ", ";
            first = false;
            oss << name;
        }
        oss << "]";
        LOG_INFO << oss.str();
    }
    
    // 只校验关键字段，不校验所有 schema 中标记为 required 的字段
    // 原因：
    // 1. 客户端 schema（如 RooCode）可能将所有字段都标记为 required
    // 2. 但某些字段是条件性必需的（如 indentation 只在 mode="indentation" 时需要）
    // 3. 通过 XML bridge 的 AI 模型可能不会提供所有 schema 要求的字段
    // 4. 我们只需要确保关键字段（path, content 等）存在
    for (const auto& req : required) {
        if (!req.isString()) continue;
        
        const std::string fieldName = req.asString();
        
        // 只校验当前客户端的关键字段
        if (!isCriticalField(toolName, fieldName)) {
            LOG_INFO << "[ToolCallValidator] 跳过非关键字段: " << fieldName;
            continue;
        }
        
        LOG_INFO << "[ToolCallValidator] 校验关键字段: " << fieldName
                  << ", 存在=" << (args.isMember(fieldName) ? "是" : "否");
        
        if (!args.isMember(fieldName)) {
            return ValidationResult::failure(
                "工具 '" + toolName + "' 缺少必需字段: " + fieldName
            );
        }
        
        // 检查字段是否为 null
        if (args[fieldName].isNull()) {
            return ValidationResult::failure(
                "工具 '" + toolName + "' 必需字段 '" + fieldName + "' 为 null"
            );
        }
    }
    
    return ValidationResult::success();
}

ValidationResult ToolCallValidator::validateFieldTypes(
    const std::string& toolName,
    const Json::Value& args,
    const Json::Value& schema
) const {
    if (!schema.isObject() || !schema.isMember("properties")) {
        return ValidationResult::success();
    }
    
    const auto& properties = schema["properties"];
    if (!properties.isObject()) {
        return ValidationResult::success();
    }
    
    for (const auto& fieldName : args.getMemberNames()) {
        if (!properties.isMember(fieldName)) {
            // 字段不在 schema 中 - 可以在这里检查 additionalProperties
            continue;
        }
        
        const auto& propSchema = properties[fieldName];
        if (!propSchema.isObject()) continue;
        
        const std::string expectedType = propSchema.get("type", "").asString();
        if (expectedType.empty()) continue;
        
        const auto& value = args[fieldName];
        bool typeMatch = false;
        
        if (expectedType == "string") {
            typeMatch = value.isString();
        } else if (expectedType == "number" || expectedType == "integer") {
            typeMatch = value.isNumeric();
        } else if (expectedType == "boolean") {
            typeMatch = value.isBool();
        } else if (expectedType == "array") {
            typeMatch = value.isArray();
        } else if (expectedType == "object") {
            typeMatch = value.isObject();
        } else {
            // 未知类型，跳过校验
            typeMatch = true;
        }
        
        if (!typeMatch) {
            return ValidationResult::failure(
                "工具 '" + toolName + "' 字段 '" + fieldName + 
                "' 类型不匹配: 期望 " + expectedType
            );
        }
    }
    
    return ValidationResult::success();
}

bool ToolCallValidator::isCriticalField(
    const std::string& toolName,
    const std::string& fieldName
) const {
    // 首先检查当前客户端的关键字段集合
    if (criticalFields_.find(fieldName) != criticalFields_.end()) {
        return true;
    }
    
    // 工具特定的关键字段（无论客户端类型）
    // 这些是每个工具正常工作所必需的最小字段
    if (toolName == "apply_diff") {
        return fieldName == "path" || fieldName == "diff";
    }
    if (toolName == "write_to_file") {
        return fieldName == "path" || fieldName == "content";
    }
    if (toolName == "read_file") {
        return fieldName == "path";
    }
    if (toolName == "execute_command") {
        return fieldName == "command";
    }
    if (toolName == "search_files") {
        return fieldName == "path" || fieldName == "regex";
    }
    if (toolName == "ask_followup_question") {
        return fieldName == "question";
    }
    if (toolName == "attempt_completion") {
        return fieldName == "result";
    }
    
    return false;
}

ValidationResult ToolCallValidator::validateCriticalFieldsNonEmpty(
    const std::string& toolName,
    const Json::Value& args
) const {
    for (const auto& fieldName : args.getMemberNames()) {
        if (!isCriticalField(toolName, fieldName)) {
            continue;
        }
        
        const auto& value = args[fieldName];
        
        // 检查字符串字段是否为空
        if (value.isString() && value.asString().empty()) {
            return ValidationResult::failure(
                "工具 '" + toolName + "' 关键字段 '" + fieldName + "' 为空"
            );
        }
    }
    
    return ValidationResult::success();
}

ValidationResult ToolCallValidator::validate(
    const generation::ToolCallDone& toolCall,
    ValidationMode mode
) const {
    // ========== ValidationMode::None - 不校验，信任 AI 输出 ==========
    // 只做最基本的检查
    if (mode == ValidationMode::None) {
        // 仍然检查工具名是否存在（基本健全性检查）
        if (toolCall.name.empty()) {
            return ValidationResult::failure("工具调用名称为空");
        }
        
        if (!hasToolDefinition(toolCall.name)) {
            return ValidationResult::failure(
                "工具 '" + toolCall.name + "' 不在工具定义中"
            );
        }
        
        // 解析参数 JSON 以确保是有效的 JSON
        Json::Value parsedArgs;
        auto parseResult = validateArgumentsParseable(toolCall.arguments, parsedArgs);
        if (!parseResult.valid) {
            return parseResult;
        }
        
        // None 模式下不做进一步校验
        LOG_INFO << "[ToolCallValidator] 模式=None, 跳过 schema 校验: " << toolCall.name;
        return ValidationResult::success();
    }
    
    // ========== Relaxed 和 Strict 模式的通用校验 ==========
    if (toolCall.name.empty()) {
        return ValidationResult::failure("工具调用名称为空");
    }
    
    if (!hasToolDefinition(toolCall.name)) {
        return ValidationResult::failure(
            "工具 '" + toolCall.name + "' 不在工具定义中"
        );
    }
    
    // 解析参数 JSON
    Json::Value parsedArgs;
    auto parseResult = validateArgumentsParseable(toolCall.arguments, parsedArgs);
    if (!parseResult.valid) {
        return parseResult;
    }
    
    // 获取工具 schema
    const Json::Value* toolDef = findToolDefinition(toolCall.name);
    if (!toolDef) {
        // 不应该发生，因为上面已经检查了 hasToolDefinition
        return ValidationResult::failure("工具定义未找到（内部错误）");
    }
    
    const auto& func = (*toolDef)["function"];
    const auto& schema = func["parameters"];
    
    // ========== ValidationMode::Relaxed - 宽松校验 ==========
    // 只校验关键字段存在且非空，跳过完整的 required 字段校验和类型检查
    if (mode == ValidationMode::Relaxed) {
        LOG_INFO << "[ToolCallValidator] 模式=Relaxed, 校验关键字段: " << toolCall.name
                  << " (客户端=" << (clientType_.empty() ? "default" : clientType_) << ")";
        
        // 检查关键字段是否存在（使用 isCriticalField 逻辑）
        auto requiredResult = validateRequiredFields(toolCall.name, parsedArgs, schema);
        if (!requiredResult.valid) {
            return requiredResult;
        }
        
        // 校验关键字段非空
        auto nonEmptyResult = validateCriticalFieldsNonEmpty(toolCall.name, parsedArgs);
        if (!nonEmptyResult.valid) {
            return nonEmptyResult;
        }
        
        return ValidationResult::success();
    }
    
    // ========== ValidationMode::Strict - 严格校验 ==========
    LOG_INFO << "[ToolCallValidator] 模式=Strict, 完整 schema 校验: " << toolCall.name;
    
    // 校验所有 required 字段（不仅仅是关键字段）
    const auto& required = schema["required"];
    if (required.isArray()) {
        for (const auto& req : required) {
            if (!req.isString()) continue;
            const std::string fieldName = req.asString();
            
            if (!parsedArgs.isMember(fieldName)) {
                return ValidationResult::failure(
                    "工具 '" + toolCall.name + "' 缺少必需字段: " + fieldName
                );
            }
            
            if (parsedArgs[fieldName].isNull()) {
                return ValidationResult::failure(
                    "工具 '" + toolCall.name + "' 必需字段 '" + fieldName + "' 为 null"
                );
            }
        }
    }
    
    // 校验字段类型
    auto typeResult = validateFieldTypes(toolCall.name, parsedArgs, schema);
    if (!typeResult.valid) {
        return typeResult;
    }
    
    // 校验关键字段非空
    auto nonEmptyResult = validateCriticalFieldsNonEmpty(toolCall.name, parsedArgs);
    if (!nonEmptyResult.valid) {
        return nonEmptyResult;
    }
    
    return ValidationResult::success();
}

size_t ToolCallValidator::filterInvalidToolCalls(
    std::vector<generation::ToolCallDone>& toolCalls,
    std::string& discardedText,
    ValidationMode mode
) const {
    size_t removedCount = 0;
    
    const char* modeStr = (mode == ValidationMode::None) ? "None" :
                          (mode == ValidationMode::Relaxed) ? "Relaxed" : "Strict";
    LOG_INFO << "[ToolCallValidator] 过滤工具调用, 模式=" << modeStr
              << ", 客户端=" << (clientType_.empty() ? "default" : clientType_);
    
    auto it = toolCalls.begin();
    while (it != toolCalls.end()) {
        auto result = validate(*it, mode);
        
        if (!result.valid) {
            LOG_WARN << "[ToolCallValidator] 丢弃无效工具调用 '"
                     << it->name << "': " << result.errorMessage;
            
            // 将丢弃的工具调用信息追加到 discardedText，用于可能的降级处理
            if (!discardedText.empty()) {
                discardedText += "\n";
            }
            discardedText += "[丢弃的工具调用: " + it->name + " - " + result.errorMessage + "]";
            
            it = toolCalls.erase(it);
            ++removedCount;
        } else {
            ++it;
        }
    }
    
    if (removedCount > 0) {
        LOG_INFO << "[ToolCallValidator] 过滤了 " << removedCount << " 个无效工具调用 (模式=" << modeStr << ")";
    }
    
    return removedCount;
}

// ============================================================================
// 全局辅助函数
// ============================================================================

bool isStrictToolClient(const std::string& clientType) {
    return clientType == "Kilo-Code" || clientType == "RooCode";
}

ValidationMode getRecommendedValidationMode(const std::string& clientType) {
    // Roo/Kilo 客户端使用 Relaxed 模式（校验关键字段）
    if (isStrictToolClient(clientType)) {
        return ValidationMode::Relaxed;
    }
    // 其他客户端使用 None 模式（不校验，信任 AI 输出）
    return ValidationMode::None;
}

FallbackStrategy applyValidationFallback(
    const std::string& clientType,
    std::vector<generation::ToolCallDone>& toolCalls,
    std::string& textContent,
    const std::string& discardedText
) {
    // 如果还有有效的工具调用，不需要降级
    if (!toolCalls.empty()) {
        return FallbackStrategy::DiscardOnly;
    }
    
    // 校验后没有剩余的工具调用
    const bool isStrict = isStrictToolClient(clientType);
    
    if (!isStrict) {
        // 非严格客户端：直接返回文本内容（不做特殊包装）
        // 如果有被丢弃的信息，记录日志但不污染输出
        if (!discardedText.empty() && !textContent.empty()) {
            LOG_INFO << "[ToolCallValidator] 非严格客户端, 丢弃的工具调用: " << discardedText;
        }
        return FallbackStrategy::DiscardOnly;
    }
    
    // 严格客户端（Roo/Kilo）：需要包装为 attempt_completion
    // 确保客户端始终收到恰好一个工具调用
    
    std::string resultText = textContent;
    if (resultText.empty()) {
        resultText = "处理工具调用时遇到问题，请重试。";
    }
    
    generation::ToolCallDone tc;
    tc.id = generateFallbackToolCallId();
    tc.name = "attempt_completion";
    tc.index = 0;
    
    Json::Value args(Json::objectValue);
    args["result"] = resultText;
    
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    tc.arguments = Json::writeString(writer, args);
    
    toolCalls.push_back(tc);
    textContent.clear();
    
    LOG_WARN << "[ToolCallValidator][" << clientType 
             << "] 所有工具调用无效, 已将文本包装为 attempt_completion";
    
    return FallbackStrategy::WrapAttemptCompletion;
}

} // namespace toolcall
