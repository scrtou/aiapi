#include "ClientOutputSanitizer.h"
#include <drogon/drogon.h>

using namespace drogon;

const std::vector<std::string>& ClientOutputSanitizer::getKiloTools() {
    static const std::vector<std::string> tools = {
        "read_file", "write_to_file", "execute_command", "search_files",
        "list_files", "attempt_completion", "ask_followup_question",
        "switch_mode", "new_task", "update_todo_list", "fetch_instructions",
        "apply_diff", "delete_file"
    };
    return tools;
}

void ClientOutputSanitizer::replaceAll(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

bool ClientOutputSanitizer::needsSanitize(const Json::Value& clientInfo) {
    std::string clientType = clientInfo.get("client_type", "").asString();
    // 目前只对 Kilo-Code 和 RooCode 客户端进行清洗
    // 注意：当前代码中条件是 "Kilo-Codetest"，这里保持一致，实际生产中可能需要改为 "Kilo-Code"
    return (clientType == "Kilo-Code" || clientType == "RooCode" || clientType == "Kilo-Codetest");
}

void ClientOutputSanitizer::fixCommonTagErrors(std::string& message) {
    // 1. 基础标签纠错 (修正模型常见的拼写错误)
    replaceAll(message, "<write_file>", "<write_to_file>");
    replaceAll(message, "</write_file>", "</write_to_file>");
    replaceAll(message, "<list_dir>", "<list_files>");
    replaceAll(message, "</list_dir>", "</list_files>");
    replaceAll(message, "<run_command>", "<execute_command>");
    replaceAll(message, "</run_command>", "</execute_command>");
}

bool ClientOutputSanitizer::fixAttemptCompletionWrapping(std::string& message) {
    const auto& kiloTools = getKiloTools();
    
    // 检测 attempt_completion 包裹其他工具调用的情况
    if (message.find("<attempt_completion>") != std::string::npos) {
        for (const auto& tool : kiloTools) {
            // 跳过 attempt_completion 自身，否则会把自己剥离掉
            if (tool == "attempt_completion") continue;

            std::string openTag = "<" + tool + ">";
            std::string closeTag = "</" + tool + ">";

            // 如果在 attempt_completion 内部发现了其他工具标签
            if (message.find(openTag) != std::string::npos) {
                LOG_INFO << "[ClientOutputSanitizer] 检测到 attempt_completion 错误包裹了工具: " << tool << "，正在剥离外层...";
                
                size_t start = message.find(openTag);
                size_t end = message.rfind(closeTag);
                
                if (start != std::string::npos && end != std::string::npos) {
                    end += closeTag.length(); 
                    if (end > start) {
                        // 提取内部工具，丢弃外层的 attempt_completion
                        message = message.substr(start, end - start);
                        LOG_INFO << "[ClientOutputSanitizer] 剥离完成，工具指令已提取。";
                        return true; // 找到一个就退出，因为 Kilo 一次只执行一个工具
                    }
                }
            }
        }
    }
    return false;
}

void ClientOutputSanitizer::fixMarkdownWrapping(std::string& message) {
    const auto& kiloTools = getKiloTools();
    
    // 检查是否包含工具标签
    bool hasDirectTool = false;
    for (const auto& tool : kiloTools) {
        if (message.find("<" + tool + ">") != std::string::npos) {
            hasDirectTool = true;
            break;
        }
    }

    if (hasDirectTool) {
        // 如果包含工具标签，检查是否被 Markdown 代码块包裹
        size_t startTag = message.find('<');
        size_t endTag = message.rfind('>');
        // 如果 < 出现在比较靠后的位置（说明前面有 ```xml 或者废话），则提取
        if (startTag != std::string::npos && endTag != std::string::npos && endTag > startTag) {
            // 简单的启发式：如果不是从 0 开始，或者结尾后面还有东西，就裁剪
            if (startTag > 0 || endTag < message.length() - 1) {
                message = message.substr(startTag, endTag - startTag + 1);
            }
        }
    } else {
        // 如果完全没有工具标签，说明是纯文本回复，强制包裹 attempt_completion 以便客户端显示
        // 防止 message 为空时包裹空标签
        if (!message.empty()) {
            message = "<attempt_completion><result>" + message + "</result></attempt_completion>";
        }
    }
}

std::string ClientOutputSanitizer::sanitize(const Json::Value& clientInfo, const std::string& text) {
    // 检查是否需要清洗
    if (!needsSanitize(clientInfo)) {
        return text;
    }
    
    if (text.empty()) {
        return text;
    }
    
    std::string message = text;
    std::string clientType = clientInfo.get("client_type", "").asString();
    
    LOG_INFO << "[ClientOutputSanitizer] 正在对 " << clientType << " 客户端的响应进行标签清洗...";
    
    // 1. 基础标签纠错
    fixCommonTagErrors(message);
    
    // 2. 检测并修复 attempt_completion 错误包裹
    if (fixAttemptCompletionWrapping(message)) {
        // 如果进行了修复，直接返回，不需要继续处理 markdown
        return message;
    }
    
    // 3. 处理 markdown 包裹
    fixMarkdownWrapping(message);
    
    return message;
}
