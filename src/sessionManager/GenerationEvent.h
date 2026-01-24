#ifndef GENERATION_EVENT_H
#define GENERATION_EVENT_H

#include <string>
#include <variant>
#include <optional>

/**
 * @brief 统一输出事件模型
 * 
 * Session/UseCase 产生"事件"（delta、done、error、usage等）。
 * Controller 只负责提供不同编码的 sink（Chat SSE / Chat JSON / Responses SSE / Responses JSON）。
 * 
 * 注意：Responses API 的"多事件序列"是编码层差异，不应要求 Session 层直接知道
 * `response.output_item.added` 等具体 eventName。Session 层只产生语义事件，
 * Sink 负责映射为协议事件序列。
 * 
 * 参考设计文档: plans/aiapi-refactor-design.md 第 4.2 节
 */

namespace generation {

// ========== 事件类型定义 ==========

/**
 * @brief 生成开始事件
 */
struct Started {
    std::string responseId;     // 生成的 response_id
    std::string model;          // 使用的模型
};

/**
 * @brief 文本增量事件
 * 
 * 在流式输出时，每次产生新的文本片段时触发
 */
struct OutputTextDelta {
    std::string delta;          // 增量文本
    int index = 0;              // 输出项索引（用于多输出）
};

/**
 * @brief 文本完成事件
 * 
 * 完整文本生成完毕时触发
 */
struct OutputTextDone {
    std::string text;           // 完整文本
    int index = 0;              // 输出项索引
};

/**
 * @brief Token 使用量
 */
struct Usage {
    int inputTokens = 0;        // 输入 token 数
    int outputTokens = 0;       // 输出 token 数
    int totalTokens = 0;        // 总 token 数
};

/**
 * @brief 生成完成事件
 */
struct Completed {
    std::string finishReason;   // 完成原因: "stop", "length", "content_filter", "tool_calls"
    std::optional<Usage> usage; // 可选的使用量信息
};

/**
 * @brief 错误代码
 * 
 * 统一错误类型，与 HTTP 状态码分离
 */
enum class ErrorCode {
    BadRequest,         // 请求格式错误 -> 400
    Unauthorized,       // 未授权 -> 401
    Forbidden,          // 禁止访问 -> 403
    NotFound,           // 资源不存在 -> 404
    Conflict,           // 冲突（如并发请求） -> 409
    RateLimited,        // 限流 -> 429
    Timeout,            // 超时 -> 504 或 408
    ProviderError,      // Provider 错误 -> 502
    Internal,           // 内部错误 -> 500
    Cancelled           // 请求被取消 -> 499
};

/**
 * @brief 错误事件
 */
struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;        // 错误信息
    std::string providerCode;   // Provider 原始错误码（可选）
    std::string detail;         // 详细信息（可选）
};

// ========== 统一事件类型 ==========

/**
 * @brief 生成事件
 * 
 * 使用 std::variant 实现类型安全的事件多态
 */
using GenerationEvent = std::variant<
    Started,
    OutputTextDelta,
    OutputTextDone,
    Usage,
    Completed,
    Error
>;

// ========== 辅助函数 ==========

/**
 * @brief 判断事件是否为终结事件
 * 
 * @param event 事件
 * @return true 如果是 Completed 或 Error
 */
inline bool isTerminalEvent(const GenerationEvent& event) {
    return std::holds_alternative<Completed>(event) || 
           std::holds_alternative<Error>(event);
}

/**
 * @brief 获取事件类型名称（用于日志）
 */
inline std::string getEventTypeName(const GenerationEvent& event) {
    if (std::holds_alternative<Started>(event)) return "Started";
    if (std::holds_alternative<OutputTextDelta>(event)) return "OutputTextDelta";
    if (std::holds_alternative<OutputTextDone>(event)) return "OutputTextDone";
    if (std::holds_alternative<Usage>(event)) return "Usage";
    if (std::holds_alternative<Completed>(event)) return "Completed";
    if (std::holds_alternative<Error>(event)) return "Error";
    return "Unknown";
}

/**
 * @brief ErrorCode 转换为 HTTP 状态码
 */
inline int errorCodeToHttpStatus(ErrorCode code) {
    switch (code) {
        case ErrorCode::BadRequest:     return 400;
        case ErrorCode::Unauthorized:   return 401;
        case ErrorCode::Forbidden:      return 403;
        case ErrorCode::NotFound:       return 404;
        case ErrorCode::Conflict:       return 409;
        case ErrorCode::RateLimited:    return 429;
        case ErrorCode::Timeout:        return 504;
        case ErrorCode::ProviderError:  return 502;
        case ErrorCode::Internal:       return 500;
        case ErrorCode::Cancelled:      return 499;
        default:                        return 500;
    }
}

/**
 * @brief ErrorCode 转换为字符串
 */
inline std::string errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::BadRequest:     return "bad_request";
        case ErrorCode::Unauthorized:   return "unauthorized";
        case ErrorCode::Forbidden:      return "forbidden";
        case ErrorCode::NotFound:       return "not_found";
        case ErrorCode::Conflict:       return "conflict";
        case ErrorCode::RateLimited:    return "rate_limited";
        case ErrorCode::Timeout:        return "timeout";
        case ErrorCode::ProviderError:  return "provider_error";
        case ErrorCode::Internal:       return "internal_error";
        case ErrorCode::Cancelled:      return "cancelled";
        default:                        return "unknown_error";
    }
}

} // namespace generation

#endif // GENERATION_EVENT_H
