#ifndef BRIDGE_HELPERS_H
#define BRIDGE_HELPERS_H

#include "sessionManager/core/Session.h"
#include "sessionManager/contracts/GenerationEvent.h"
#include <metrics/ErrorStatsService.h>
#include <metrics/ErrorEvent.h>
#include <json/json.h>
#include <string>
#include <string_view>
#include <vector>

namespace bridge {

// ========== 字符串工具函数 ==========

std::string trimWhitespace(std::string s);
std::string stripMarkdownCodeFence(std::string s);
std::string toLowerStr(std::string s);
std::string safeJsonAsString(const Json::Value& val, const std::string& defaultVal = "");
std::string_view ltrimView(const std::string& s);
bool startsWithStr(std::string_view s, std::string_view prefix);



std::string normalizeBridgeXml(std::string s);
std::string extractXmlInputForToolCalls(const session_st& session, const std::string& rawText);



std::string generateFallbackToolCallId();
std::string generateRandomTriggerSignal();

// ========== 错误统计辅助函数 ==========

std::string getClientTypeFromSession(const session_st& session);
std::string getApiKindFromSession(const session_st& session);

void recordErrorStat(
    const session_st& session,
    metrics::Domain domain,
    const std::string& type,
    const std::string& message,
    int httpStatus = 0,
    const Json::Value& detail = Json::Value(),
    const std::string& rawSnippet = "",
    const std::string& toolName = ""
);

void recordWarnStat(
    const session_st& session,
    metrics::Domain domain,
    const std::string& type,
    const std::string& message,
    const Json::Value& detail = Json::Value(),
    const std::string& rawSnippet = "",
    const std::string& toolName = ""
);

void recordRequestCompletedStat(const session_st& session, int httpStatus);

} // 命名空间 桥接

#endif
