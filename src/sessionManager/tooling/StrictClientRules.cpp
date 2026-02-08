#include "sessionManager/tooling/StrictClientRules.h"
#include <drogon/drogon.h>
#include <json/json.h>
#include <iomanip>
#include <random>
#include <sstream>

using namespace drogon;

namespace {

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

}

namespace toolcall {

void applyStrictClientRules(
    const std::string& clientType,
    std::string& textContent,
    std::vector<generation::ToolCallDone>& toolCalls
) {
    // 规则 1： 如果没有工具调用但有文本，包装为 attempt_completion
    if (toolCalls.empty() && !textContent.empty()) {
        generation::ToolCallDone tc;
        tc.id = generateFallbackToolCallId();
        tc.name = "attempt_completion";
        tc.index = 0;

        Json::Value args(Json::objectValue);
        args["result"] = textContent;
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        tc.arguments = Json::writeString(writer, args);

        toolCalls.push_back(tc);
        LOG_WARN << "[严格客户端规则][" << clientType << "] 未检测到工具调用，已自动包装为 attempt_completion";

        textContent.clear();
    } else if (!toolCalls.empty()) {
        // 规则 2: 如果有工具调用，保持文本内容（已注释掉清空逻辑）
    }

    // 规则 3: 如果有多个工具调用，只保留第一个
    if (toolCalls.size() > 1) {
        LOG_WARN << "[严格客户端规则][" << clientType << "] 检测到多个工具调用，已仅保留第一个以满足客户端约束";
        toolCalls.erase(toolCalls.begin() + 1, toolCalls.end());
    }
}

}
