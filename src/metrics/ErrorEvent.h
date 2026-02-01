#ifndef ERROR_EVENT_H
#define ERROR_EVENT_H

#include <string>
#include <chrono>
#include <json/json.h>
#include <cstdint>

namespace metrics {

/**
 * @brief 事件等级
 * 
 * - ERROR：请求失败或返回终止错误
 * - WARN：请求可能成功，但出现异常/降级/自愈/不符合预期的情况
 */
enum class Severity {
    WARN,
    ERROR
};

/**
 * @brief 事件域
 * 
 * 用于前端快速过滤与 Prometheus 维度分类
 */
enum class Domain {
    UPSTREAM,        // 向 Provider 发起请求/处理响应的错误
    TOOL_BRIDGE,     // 文本转工具链路（注入、sentinel、XML 解析、参数归一、校验、降级、自愈等）
    TOOL_VALIDATION, // ToolCallValidator 校验相关
    SESSION_GATE,    // 并发门控（409 RejectConcurrent、CancelPrevious 取消等）
    INTERNAL,        // 内部异常
    REQUEST          // 请求输入不合法
};

/**
 * @brief 事件类型常量
 * 
 * 使用字符串常量便于扩展和聚合统计
 * 类型要"少而稳定"，细节放到 detail_json
 */
namespace EventType {
    // ========== UPSTREAM ==========
    constexpr const char* UPSTREAM_NETWORK_ERROR = "upstream.network_error";
    constexpr const char* UPSTREAM_TIMEOUT = "upstream.timeout";
    constexpr const char* UPSTREAM_RATE_LIMITED = "upstream.rate_limited";
    constexpr const char* UPSTREAM_AUTH_ERROR = "upstream.auth_error";
    constexpr const char* UPSTREAM_HTTP_ERROR = "upstream.http_error";
    constexpr const char* UPSTREAM_SERVICE_UNAVAILABLE = "upstream.service_unavailable";
    
    // ========== TOOL_BRIDGE ==========
    constexpr const char* TOOLBRIDGE_TRANSFORM_INJECTED = "toolbridge.transform_injected";
    constexpr const char* TOOLBRIDGE_TRIGGER_MISSING = "toolbridge.trigger_missing";
    constexpr const char* TOOLBRIDGE_TRIGGER_MISMATCH_FALLBACK = "toolbridge.trigger_mismatch_fallback";
    constexpr const char* TOOLBRIDGE_XML_NOT_FOUND = "toolbridge.xml_not_found";
    constexpr const char* TOOLBRIDGE_XML_PARSE_ERROR = "toolbridge.xml_parse_error";
    constexpr const char* TOOLBRIDGE_SENTINEL_MISMATCH = "toolbridge.sentinel_mismatch";
    constexpr const char* TOOLBRIDGE_ARGS_JSON_PARSE_ERROR = "toolbridge.args_json_parse_error";
    constexpr const char* TOOLBRIDGE_NORMALIZE_APPLIED = "toolbridge.normalize_applied";
    constexpr const char* TOOLBRIDGE_FORCED_TOOLCALL_GENERATED = "toolbridge.forced_toolcall_generated";
    constexpr const char* TOOLBRIDGE_VALIDATION_FILTERED = "toolbridge.validation_filtered";
    constexpr const char* TOOLBRIDGE_VALIDATION_FALLBACK_APPLIED = "toolbridge.validation_fallback_applied";
    constexpr const char* TOOLBRIDGE_SELFHEAL_READ_FILE_APPLIED = "toolbridge.selfheal_read_file_applied";
    constexpr const char* TOOLBRIDGE_STRICT_CLIENT_RULE_APPLIED = "toolbridge.strict_client_rule_applied";
    
    // ========== TOOL_VALIDATION ==========
    constexpr const char* TOOLVALIDATION_TOOL_NOT_FOUND = "toolvalidation.tool_not_found";
    constexpr const char* TOOLVALIDATION_ARGUMENTS_NOT_OBJECT = "toolvalidation.arguments_not_object";
    constexpr const char* TOOLVALIDATION_REQUIRED_FIELD_MISSING = "toolvalidation.required_field_missing";
    constexpr const char* TOOLVALIDATION_FIELD_TYPE_MISMATCH = "toolvalidation.field_type_mismatch";
    constexpr const char* TOOLVALIDATION_CRITICAL_FIELD_EMPTY = "toolvalidation.critical_field_empty";
    
    // ========== SESSION_GATE ==========
    constexpr const char* SESSIONGATE_REJECTED_CONFLICT = "sessiongate.rejected_conflict";
    constexpr const char* SESSIONGATE_CANCELLED = "sessiongate.cancelled";
    
    // ========== INTERNAL ==========
    constexpr const char* INTERNAL_EXCEPTION = "internal.exception";
    constexpr const char* INTERNAL_UNKNOWN = "internal.unknown";
}

/**
 * @brief 错误事件结构体
 * 
 * 用于记录错误/告警事件的完整信息
 */
struct ErrorEvent {
    int64_t id = 0;                                    // DB 自增 ID（写入后填充）
    std::chrono::system_clock::time_point ts;         // 事件时间（UTC）
    Severity severity = Severity::WARN;
    Domain domain = Domain::INTERNAL;
    std::string type;                                  // 事件类型（EventType::*）
    
    // ========== 聚合维度 ==========
    std::string provider;                              // API 提供者名称
    std::string model;                                 // 模型名称
    std::string clientType;                            // 客户端类型
    std::string apiKind;                               // "chat_completions" | "responses"
    bool stream = false;                               // 是否流式
    int httpStatus = 0;                                // HTTP 状态码
    
    // ========== 请求级字段 ==========
    std::string requestId;                             // 请求唯一 ID
    std::string responseId;                            // Response API ID（可选）
    std::string toolName;                              // 工具名（可选）
    
    // ========== 详情 ==========
    std::string message;                               // 简短信息（可展示）
    Json::Value detailJson;                            // 结构化详情
    std::string rawSnippet;                            // 原始片段（可选，如 XML 输入/args_json）
    
    // ========== 辅助方法 ==========
    
    /**
     * @brief 将 Severity 枚举转换为字符串
     */
    static std::string severityToString(Severity s) {
        switch (s) {
            case Severity::WARN: return "WARN";
            case Severity::ERROR: return "ERROR";
            default: return "UNKNOWN";
        }
    }
    
    /**
     * @brief 将字符串转换为 Severity 枚举
     */
    static Severity stringToSeverity(const std::string& s) {
        if (s == "ERROR") return Severity::ERROR;
        return Severity::WARN;  // 默认 WARN
    }
    
    /**
     * @brief 将 Domain 枚举转换为字符串
     */
    static std::string domainToString(Domain d) {
        switch (d) {
            case Domain::UPSTREAM: return "UPSTREAM";
            case Domain::TOOL_BRIDGE: return "TOOL_BRIDGE";
            case Domain::TOOL_VALIDATION: return "TOOL_VALIDATION";
            case Domain::SESSION_GATE: return "SESSION_GATE";
            case Domain::INTERNAL: return "INTERNAL";
            case Domain::REQUEST: return "REQUEST";
            default: return "UNKNOWN";
        }
    }
    
    /**
     * @brief 将字符串转换为 Domain 枚举
     */
    static Domain stringToDomain(const std::string& s) {
        if (s == "UPSTREAM") return Domain::UPSTREAM;
        if (s == "TOOL_BRIDGE") return Domain::TOOL_BRIDGE;
        if (s == "TOOL_VALIDATION") return Domain::TOOL_VALIDATION;
        if (s == "SESSION_GATE") return Domain::SESSION_GATE;
        if (s == "INTERNAL") return Domain::INTERNAL;
        if (s == "REQUEST") return Domain::REQUEST;
        return Domain::INTERNAL;  // 默认 INTERNAL
    }
    
    /**
     * @brief 转换为 JSON 对象（用于 API 返回）
     */
    Json::Value toJson() const {
        Json::Value json;
        json["id"] = static_cast<Json::Int64>(id);
        json["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            ts.time_since_epoch()).count();
        json["severity"] = severityToString(severity);
        json["domain"] = domainToString(domain);
        json["type"] = type;
        json["provider"] = provider;
        json["model"] = model;
        json["client_type"] = clientType;
        json["api_kind"] = apiKind;
        json["stream"] = stream;
        json["http_status"] = httpStatus;
        json["request_id"] = requestId;
        json["response_id"] = responseId;
        json["tool_name"] = toolName;
        json["message"] = message;
        json["detail_json"] = detailJson;
        json["raw_snippet"] = rawSnippet;
        return json;
    }
};

/**
 * @brief 请求完成事件
 * 
 * 用于 request_agg_hour 聚合表，记录每个请求的完成信息
 */
struct RequestCompletedEvent {
    std::chrono::system_clock::time_point ts;         // 请求完成时间（UTC）
    std::string provider;                              // API 提供者名称
    std::string model;                                 // 模型名称
    std::string clientType;                            // 客户端类型
    std::string apiKind;                               // "chat_completions" | "responses"
    bool stream = false;                               // 是否流式
    int httpStatus = 0;                                // HTTP 状态码
};

} // namespace metrics

#endif // ERROR_EVENT_H
