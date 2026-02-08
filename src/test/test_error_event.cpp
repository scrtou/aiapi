/**
 * @file test_error_event.cpp
 * @brief ErrorEvent 单元测试
 * 
 * 测试内容：
 * - Severity 枚举转换
 * - Domain 枚举转换
 * - ErrorEvent 序列化/反序列化
 */

#include <drogon/drogon_test.h>
#include "../metrics/ErrorEvent.h"

using namespace metrics;

// ========== 枚举转换测试 ==========

DROGON_TEST(ErrorEvent_SeverityToString)
{
    CHECK(ErrorEvent::severityToString(Severity::WARN) == "WARN");
    CHECK(ErrorEvent::severityToString(Severity::ERROR) == "ERROR");
}

DROGON_TEST(ErrorEvent_StringToSeverity)
{
    CHECK(ErrorEvent::stringToSeverity("WARN") == Severity::WARN);
    CHECK(ErrorEvent::stringToSeverity("ERROR") == Severity::ERROR);
    CHECK(ErrorEvent::stringToSeverity("unknown") == Severity::WARN);  // 默认 警告
    CHECK(ErrorEvent::stringToSeverity("") == Severity::WARN);
}

// ========== 枚举转换测试 ==========

DROGON_TEST(ErrorEvent_DomainToString)
{
    CHECK(ErrorEvent::domainToString(Domain::UPSTREAM) == "UPSTREAM");
    CHECK(ErrorEvent::domainToString(Domain::TOOL_BRIDGE) == "TOOL_BRIDGE");
    CHECK(ErrorEvent::domainToString(Domain::TOOL_VALIDATION) == "TOOL_VALIDATION");
    CHECK(ErrorEvent::domainToString(Domain::SESSION_GATE) == "SESSION_GATE");
    CHECK(ErrorEvent::domainToString(Domain::INTERNAL) == "INTERNAL");
    CHECK(ErrorEvent::domainToString(Domain::REQUEST) == "REQUEST");
}

DROGON_TEST(ErrorEvent_StringToDomain)
{
    CHECK(ErrorEvent::stringToDomain("UPSTREAM") == Domain::UPSTREAM);
    CHECK(ErrorEvent::stringToDomain("TOOL_BRIDGE") == Domain::TOOL_BRIDGE);
    CHECK(ErrorEvent::stringToDomain("TOOL_VALIDATION") == Domain::TOOL_VALIDATION);
    CHECK(ErrorEvent::stringToDomain("SESSION_GATE") == Domain::SESSION_GATE);
    CHECK(ErrorEvent::stringToDomain("INTERNAL") == Domain::INTERNAL);
    CHECK(ErrorEvent::stringToDomain("REQUEST") == Domain::REQUEST);
    CHECK(ErrorEvent::stringToDomain("unknown") == Domain::INTERNAL);  // 默认 INTERNAL
}

// ========== 错误Event 序列化测试 ==========

DROGON_TEST(ErrorEvent_ToJson)
{
    ErrorEvent event;
    event.id = 123;
    event.ts = std::chrono::system_clock::now();
    event.severity = Severity::ERROR;
    event.domain = Domain::UPSTREAM;
    event.type = "upstream.timeout";
    event.provider = "openai";
    event.model = "gpt-4";
    event.clientType = "claude-code";
    event.apiKind = "chat_completions";
    event.stream = true;
    event.httpStatus = 504;
    event.requestId = "req_123";
    event.responseId = "resp_456";
    event.toolName = "";
    event.message = "Request timeout";
    event.detailJson["timeout_ms"] = 30000;
    event.rawSnippet = "";
    
    Json::Value json = event.toJson();
    
    CHECK(json["id"].asInt64() == 123);
    CHECK(json["severity"].asString() == "ERROR");
    CHECK(json["domain"].asString() == "UPSTREAM");
    CHECK(json["type"].asString() == "upstream.timeout");
    CHECK(json["provider"].asString() == "openai");
    CHECK(json["model"].asString() == "gpt-4");
    CHECK(json["client_type"].asString() == "claude-code");
    CHECK(json["api_kind"].asString() == "chat_completions");
    CHECK(json["stream"].asBool() == true);
    CHECK(json["http_status"].asInt() == 504);
    CHECK(json["request_id"].asString() == "req_123");
    CHECK(json["response_id"].asString() == "resp_456");
    CHECK(json["message"].asString() == "Request timeout");
}



DROGON_TEST(EventType_Constants)
{
    // 验证事件类型常量存在且格式正确
    CHECK(std::string(EventType::UPSTREAM_NETWORK_ERROR) == "upstream.network_error");
    CHECK(std::string(EventType::UPSTREAM_TIMEOUT) == "upstream.timeout");
    CHECK(std::string(EventType::UPSTREAM_RATE_LIMITED) == "upstream.rate_limited");
    CHECK(std::string(EventType::TOOLBRIDGE_XML_NOT_FOUND) == "toolbridge.xml_not_found");
    CHECK(std::string(EventType::SESSIONGATE_REJECTED_CONFLICT) == "sessiongate.rejected_conflict");
    CHECK(std::string(EventType::INTERNAL_EXCEPTION) == "internal.exception");
}
