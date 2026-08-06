#include "sessionManager/tooling/StrictClientRules.h"
#include "sessionManager/tooling/ToolDefinitionResolver.h"
#include <drogon/drogon.h>
#include <array>
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

constexpr std::array<const char*, 4> kApplyDiffFailureMarkers = {
    "No sufficiently similar match found",
    "Unable to apply diff to file",
    "But unable to apply all diff parts",
    "Invalid diff format - missing required sections"
};

bool textContainsApplyDiffFailure(const std::string& text) {
    if (text.empty()) return false;
    for (const char* marker : kApplyDiffFailureMarkers) {
        if (text.find(marker) != std::string::npos) return true;
    }
    return false;
}

bool looksLikeReadFileResult(const std::string& text) {
    const size_t firstNonSpace = text.find_first_not_of(" \t\r\n");
    if (firstNonSpace == std::string::npos || text.compare(firstNonSpace, 6, "File: ") != 0) {
        return false;
    }
    return text.find('\n', firstNonSpace) != std::string::npos &&
        text.find(" | ", firstNonSpace) != std::string::npos;
}

void collectTextValuesInOrder(const Json::Value& value, std::vector<std::string>& output) {
    if (value.isString()) {
        output.push_back(value.asString());
        return;
    }
    if (value.isArray()) {
        for (const auto& item : value) {
            collectTextValuesInOrder(item, output);
        }
        return;
    }
    if (!value.isObject()) return;

    // Message objects should be traversed through content first; this preserves
    // conversational order and avoids treating metadata as recovery evidence.
    if (value.isMember("content")) {
        collectTextValuesInOrder(value["content"], output);
        return;
    }
    if (value.isMember("text")) {
        collectTextValuesInOrder(value["text"], output);
        return;
    }

    for (const auto& name : value.getMemberNames()) {
        collectTextValuesInOrder(value[name], output);
    }
}

void updateApplyDiffRecoveryState(const std::string& text, bool& pendingRecovery) {
    if (textContainsApplyDiffFailure(text)) {
        pendingRecovery = true;
        return;
    }
    if (pendingRecovery && looksLikeReadFileResult(text)) {
        pendingRecovery = false;
    }
}

}

namespace toolcall {

bool hasToolNamed(const Json::Value& tools, const std::string& toolName) {
    if (toolName.empty()) return false;

    bool found = false;
    visitToolDefinitions(tools, [&](const ToolDefinitionMatch& match) {
        if (match.bridgeName != toolName && match.originalName != toolName) {
            return true;
        }
        found = true;
        return false;
    });
    return found;
}

bool hasApplyDiffFailureContext(
    const Json::Value& messageContext,
    const std::string& currentMessage,
    const std::string& rawMessage
) {
    std::vector<std::string> orderedTexts;
    collectTextValuesInOrder(messageContext, orderedTexts);

    bool pendingRecovery = false;
    for (const auto& text : orderedTexts) {
        updateApplyDiffRecoveryState(text, pendingRecovery);
    }

    // currentMessage/rawMessage can carry the latest tool result depending on
    // the request adapter. Inspect them last, but avoid processing duplicates.
    updateApplyDiffRecoveryState(currentMessage, pendingRecovery);
    if (rawMessage != currentMessage) {
        updateApplyDiffRecoveryState(rawMessage, pendingRecovery);
    }
    return pendingRecovery;
}

std::string buildStrictApplyDiffPolicy(bool recoveringFromFailure) {
    std::ostringstream policy;
    policy << "\nStrict apply_diff safety rules for RooCode/Kilo-Code:\n";
    policy << "- Use only Roo SEARCH/REPLACE blocks (<<<<<<< SEARCH ... ======= ... >>>>>>> REPLACE); never use ---/+++ Unified Diff syntax.\n";
    policy << "- Copy every SEARCH line verbatim from the latest read_file result or the tool error's Best Match Found block.\n";
    policy << "- SEARCH matching is case-sensitive and requires a 100% exact match. Preserve capitalization, whitespace, punctuation, and Unicode characters.\n";
    policy << "- Never normalize or rewrite identifiers, comments, paths, or filenames inside SEARCH text.\n";
    policy << "- Verify identifiers and filename capitalization from the latest read/list result before placing them in REPLACE text.\n";
    policy << "- Prefer a small block for one logical edit instead of combining unrelated edits.\n";

    if (recoveringFromFailure) {
        policy << "\nRecovery required: a previous apply_diff attempt failed or only partially modified the file.\n";
        policy << "- Do NOT resend the same path+diff payload.\n";
        policy << "- Your next tool call should read_file the smallest affected range before retrying the edit.\n";
        policy << "- If the error contains Best Match Found, copy its relevant lines exactly; do not paraphrase or change case.\n";
        policy << "- If the result says operation: modified but not all parts applied, treat the file as changed and re-read it before any retry.\n";
    }

    return policy.str();
}

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
