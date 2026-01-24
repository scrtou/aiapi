#ifndef ERRORS_H
#define ERRORS_H

#include <string>
#include <optional>

/**
 * @brief 统一错误模型
 * 
 * 定义应用层错误类型，与 HTTP 状态码映射。
 * 
 * 参考设计文档: plans/aiapi-refactor-design.md 第 8 节
 */

namespace error {

/**
 * @brief 错误码枚举
 */
enum class ErrorCode {
    None = 0,           // 无错误
    BadRequest,         // 400 - 请求格式错误
    Unauthorized,       // 401 - 未授权
    Forbidden,          // 403 - 禁止访问
    NotFound,           // 404 - 资源不存在
    Conflict,           // 409 - 冲突（如并发请求）
    RateLimited,        // 429 - 请求过于频繁
    Timeout,            // 504 - 超时
    ProviderError,      // 502 - 上游服务错误
    Internal,           // 500 - 内部错误
    Cancelled           // 499 - 请求被取消
};

/**
 * @brief 错误码转字符串
 */
inline std::string errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::None: return "none";
        case ErrorCode::BadRequest: return "bad_request";
        case ErrorCode::Unauthorized: return "unauthorized";
        case ErrorCode::Forbidden: return "forbidden";
        case ErrorCode::NotFound: return "not_found";
        case ErrorCode::Conflict: return "conflict";
        case ErrorCode::RateLimited: return "rate_limited";
        case ErrorCode::Timeout: return "timeout";
        case ErrorCode::ProviderError: return "provider_error";
        case ErrorCode::Internal: return "internal_error";
        case ErrorCode::Cancelled: return "cancelled";
        default: return "unknown";
    }
}

/**
 * @brief 错误码转 HTTP 状态码
 */
inline int errorCodeToHttpStatus(ErrorCode code) {
    switch (code) {
        case ErrorCode::None: return 200;
        case ErrorCode::BadRequest: return 400;
        case ErrorCode::Unauthorized: return 401;
        case ErrorCode::Forbidden: return 403;
        case ErrorCode::NotFound: return 404;
        case ErrorCode::Conflict: return 409;
        case ErrorCode::RateLimited: return 429;
        case ErrorCode::Timeout: return 504;
        case ErrorCode::ProviderError: return 502;
        case ErrorCode::Internal: return 500;
        case ErrorCode::Cancelled: return 499;
        default: return 500;
    }
}

/**
 * @brief 应用错误结构
 */
struct AppError {
    ErrorCode code = ErrorCode::None;
    std::string message;
    std::string detail;
    std::string providerCode;  // 上游错误码（可选）
    
    AppError() = default;
    
    AppError(ErrorCode c, const std::string& msg, const std::string& det = "")
        : code(c), message(msg), detail(det) {}
    
    /**
     * @brief 是否有错误
     */
    bool hasError() const {
        return code != ErrorCode::None;
    }
    
    /**
     * @brief 获取 HTTP 状态码
     */
    int httpStatus() const {
        return errorCodeToHttpStatus(code);
    }
    
    /**
     * @brief 获取错误类型字符串
     */
    std::string type() const {
        return errorCodeToString(code);
    }
    
    // 工厂方法
    static AppError badRequest(const std::string& msg, const std::string& det = "") {
        return AppError(ErrorCode::BadRequest, msg, det);
    }
    
    static AppError unauthorized(const std::string& msg = "Unauthorized") {
        return AppError(ErrorCode::Unauthorized, msg);
    }
    
    static AppError forbidden(const std::string& msg = "Forbidden") {
        return AppError(ErrorCode::Forbidden, msg);
    }
    
    static AppError notFound(const std::string& msg = "Resource not found") {
        return AppError(ErrorCode::NotFound, msg);
    }
    
    static AppError conflict(const std::string& msg = "Request conflict") {
        return AppError(ErrorCode::Conflict, msg);
    }
    
    static AppError rateLimited(const std::string& msg = "Too many requests") {
        return AppError(ErrorCode::RateLimited, msg);
    }
    
    static AppError timeout(const std::string& msg = "Request timeout") {
        return AppError(ErrorCode::Timeout, msg);
    }
    
    static AppError providerError(const std::string& msg, const std::string& providerCode = "") {
        AppError err(ErrorCode::ProviderError, msg);
        err.providerCode = providerCode;
        return err;
    }
    
    static AppError internal(const std::string& msg = "Internal server error") {
        return AppError(ErrorCode::Internal, msg);
    }
    
    static AppError cancelled(const std::string& msg = "Request cancelled") {
        return AppError(ErrorCode::Cancelled, msg);
    }
};

/**
 * @brief 将 generation::ErrorCode 转换为 AppError
 * 
 * @param genCode generation 层的错误码
 * @param message 错误消息
 * @return AppError 应用层错误
 */
inline AppError fromGenerationError(int genCode, const std::string& message) {
    // generation::ErrorCode 映射
    // 0: Unknown -> Internal
    // 1: ProviderError -> ProviderError
    // 2: AuthenticationError -> Unauthorized
    // 3: RateLimitError -> RateLimited
    // 4: InvalidRequest -> BadRequest
    // 5: NetworkError -> ProviderError
    // 6: Timeout -> Timeout
    // 7: Cancelled -> Cancelled
    
    switch (genCode) {
        case 0: return AppError::internal(message);
        case 1: return AppError::providerError(message);
        case 2: return AppError::unauthorized(message);
        case 3: return AppError::rateLimited(message);
        case 4: return AppError::badRequest(message);
        case 5: return AppError::providerError(message);
        case 6: return AppError::timeout(message);
        case 7: return AppError::cancelled(message);
        default: return AppError::internal(message);
    }
}

} // namespace error

#endif // ERRORS_H
