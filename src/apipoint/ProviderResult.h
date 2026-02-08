#ifndef PROVIDER_RESULT_H
#define PROVIDER_RESULT_H

#include <string>
#include <optional>
#include <vector>

/**
 * @brief Provider 层返回的结构化结果
 * 
 * 替代原有的隐式写入 session.response.message 的方式，
 * 使 Provider 返回显式的结构化数据。
 * 
 * 参考设计文档: plans/aiapi-refactor-design.md 第 6.2 节
 */
namespace provider {

/**
 * @brief Provider 层错误码
 */
enum class ProviderErrorCode {
    None = 0,           // 无错误
    NetworkError,       // 网络错误
    AuthError,          // 认证错误
    RateLimited,        // 限流
    InvalidRequest,     // 无效请求
    Timeout,            // 超时
    ServiceUnavailable, // 服务不可用
    InternalError,      // 内部错误
    Unknown             // 未知错误
};

/**
 * @brief Provider 层错误信息
 */
struct ProviderError {
    ProviderErrorCode code = ProviderErrorCode::None;
    std::string message;        // 错误消息
    std::string providerCode;   // 上游 原始错误码
    int httpStatusCode = 0;     // HTTP 状态码（如果适用）
    
    bool hasError() const {
        return code != ProviderErrorCode::None;
    }
    
    static ProviderError none() {
        return ProviderError{ProviderErrorCode::None, "", "", 0};
    }
    
    static ProviderError network(const std::string& msg) {
        return ProviderError{ProviderErrorCode::NetworkError, msg, "", 0};
    }
    
    static ProviderError auth(const std::string& msg) {
        return ProviderError{ProviderErrorCode::AuthError, msg, "", 401};
    }
    
    static ProviderError rateLimited(const std::string& msg) {
        return ProviderError{ProviderErrorCode::RateLimited, msg, "", 429};
    }
    
    static ProviderError timeout(const std::string& msg) {
        return ProviderError{ProviderErrorCode::Timeout, msg, "", 504};
    }
    
    static ProviderError internal(const std::string& msg) {
        return ProviderError{ProviderErrorCode::InternalError, msg, "", 500};
    }
};

/**
 * @brief Token 使用量统计
 */
struct Usage {
    int inputTokens = 0;
    int outputTokens = 0;
    int totalTokens = 0;
    
    bool isValid() const {
        return totalTokens > 0;
    }
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

/**
 * @brief Provider 层返回结果
 * 
 * 包含:
 * - 生成的文本内容
 * - Token 使用量（可选）
 * - 错误信息（如果有）
 * - 原始响应数据（用于调试）
 */
struct ProviderResult {
    std::string text;                   // 生成的文本
    std::optional<Usage> usage;         // Token 使用量
    std::vector<ToolCall> toolCalls;    // 原生工具调用（可选）
    ProviderError error;                // 错误信息

    // Provider 返回字段
    int statusCode = 200;               // HTTP 状态码
    std::string rawResponse;            // 原始响应（调试用）

    /**
     * @brief 判断是否成功
     */
    bool isSuccess() const {
        return !error.hasError() && statusCode == 200;
    }
    
    /**
     * @brief 创建成功结果
     */
    static ProviderResult success(const std::string& text) {
        ProviderResult result;
        result.text = text;
        result.statusCode = 200;
        result.error = ProviderError::none();
        return result;
    }
    
    /**
     * @brief 创建带 Usage 的成功结果
     */
    static ProviderResult successWithUsage(const std::string& text, const Usage& usage) {
        ProviderResult result = success(text);
        result.usage = usage;
        return result;
    }
    
    /**
     * @brief 创建错误结果
     */
    static ProviderResult fail(const ProviderError& err) {
        ProviderResult result;
        result.error = err;
        result.statusCode = err.httpStatusCode > 0 ? err.httpStatusCode : 500;
        return result;
    }
    
};

} // 命名空间结束

#endif // 头文件保护结束
