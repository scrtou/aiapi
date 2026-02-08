#include "sessionManager/tooling/BridgeHelpers.h"
#include <drogon/drogon.h>
#include <iomanip>
#include <random>
#include <sstream>

using namespace drogon;

namespace bridge {

// ========== 字符串工具函数 ==========

std::string trimWhitespace(std::string s) {
    const auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::string stripMarkdownCodeFence(std::string s) {
    s = trimWhitespace(std::move(s));
    if (s.rfind("```", 0) != 0) return s;
    size_t firstNl = s.find('\n');
    if (firstNl == std::string::npos) return s;
    size_t lastFence = s.rfind("```");
    if (lastFence == std::string::npos || lastFence <= firstNl) return s;
    return trimWhitespace(s.substr(firstNl + 1, lastFence - (firstNl + 1)));
}

std::string toLowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string safeJsonAsString(const Json::Value& val, const std::string& defaultVal) {
    if (val.isString()) return val.asString();
    if (val.isNull()) return defaultVal;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, val);
}

std::string_view ltrimView(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    return std::string_view(s).substr(i);
}

bool startsWithStr(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}



static void replaceAllBytes(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string normalizeBridgeXml(std::string s) {
    replaceAllBytes(s, "\r\n", "\n");
    replaceAllBytes(s, "\r", "\n");
    replaceAllBytes(s, "\xC2\xA0", " ");
    replaceAllBytes(s, "\xE3\x80\x80", " ");
    return s;
}

std::string extractXmlInputForToolCalls(const session_st& session, const std::string& rawText) {
    std::string xmlCandidate = stripMarkdownCodeFence(rawText);
    const std::string_view trimmed = ltrimView(xmlCandidate);

    if (!session.provider.toolBridgeTrigger.empty()) {
        size_t triggerPos = trimmed.find(session.provider.toolBridgeTrigger);
        if (triggerPos != std::string::npos) {
            return std::string(trimmed.substr(triggerPos));
        }
    }

    size_t tagPos = trimmed.find("<function_calls");
    if (tagPos != std::string::npos) {
        return std::string(trimmed.substr(tagPos));
    }

    return "";
}



std::string generateFallbackToolCallId() {
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

std::string generateRandomTriggerSignal() {
    static const std::string kChars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, static_cast<int>(kChars.size() - 1));
    std::string randomStr;
    randomStr.reserve(4);
    for (int i = 0; i < 4; ++i) {
        randomStr.push_back(kChars[dis(gen)]);
    }
    return "<Function_" + randomStr + "_Start/>";
}

// ========== 错误统计辅助函数 ==========

std::string getClientTypeFromSession(const session_st& session) {
    return safeJsonAsString(session.provider.clientInfo.get("client_type", ""), "");
}

std::string getApiKindFromSession(const session_st& session) {
    return session.isResponseApi() ? "responses" : "chat_completions";
}

void recordErrorStat(
    const session_st& session,
    metrics::Domain domain,
    const std::string& type,
    const std::string& message,
    int httpStatus,
    const Json::Value& detail,
    const std::string& rawSnippet,
    const std::string& toolName
) {
    metrics::ErrorStatsService::getInstance().recordError(
        domain, type, message,
        session.state.requestId, session.request.api, session.request.model,
        getClientTypeFromSession(session), getApiKindFromSession(session),
        false, httpStatus, detail, rawSnippet, toolName
    );
}

void recordWarnStat(
    const session_st& session,
    metrics::Domain domain,
    const std::string& type,
    const std::string& message,
    const Json::Value& detail,
    const std::string& rawSnippet,
    const std::string& toolName
) {
    metrics::ErrorStatsService::getInstance().recordWarn(
        domain, type, message,
        session.state.requestId, session.request.api, session.request.model,
        getClientTypeFromSession(session), getApiKindFromSession(session),
        false, 0, detail, rawSnippet, toolName
    );
}

void recordRequestCompletedStat(const session_st& session, int httpStatus) {
    metrics::RequestCompletedData data;
    data.provider = session.request.api;
    data.model = session.request.model;
    data.clientType = getClientTypeFromSession(session);
    data.apiKind = getApiKindFromSession(session);
    data.stream = false;
    data.httpStatus = httpStatus;
    data.ts = std::chrono::system_clock::now();
    metrics::ErrorStatsService::getInstance().recordRequestCompleted(data);
}

} // 命名空间 桥接
