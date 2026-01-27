#include "ClientOutputSanitizer.h"
#include <drogon/drogon.h>

using namespace drogon;

const std::vector<std::string>& ClientOutputSanitizer::getKiloTools() {
    static const std::vector<std::string> tools = {
        "read_file", "write_to_file", "execute_command", "search_files",
        "list_files", "attempt_completion", "ask_followup_question",
        "switch_mode", "new_task", "update_todo_list", "fetch_instructions",
        "apply_diff", "delete_file", "browser_action"
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
    return false;
    // 对 Kilo-Code 和 RooCode 客户端进行清洗
    //return (clientType == "Kilo-Code" || clientType == "RooCode" || clientType == "Kilo-Codetest");
}

void ClientOutputSanitizer::fixCommonTagErrors(std::string& message) {
    // 基础标签纠错 (修正模型常见的拼写错误)
    replaceAll(message, "<write_file>", "<write_to_file>");
    replaceAll(message, "</write_file>", "</write_to_file>");
    replaceAll(message, "<list_dir>", "<list_files>");
    replaceAll(message, "</list_dir>", "</list_files>");
    replaceAll(message, "<run_command>", "<execute_command>");
    replaceAll(message, "</run_command>", "</execute_command>");
    replaceAll(message, "<read>", "<read_file>");
    replaceAll(message, "</read>", "</read_file>");
    replaceAll(message, "<write>", "<write_to_file>");
    replaceAll(message, "</write>", "</write_to_file>");
}

std::string ClientOutputSanitizer::removeControlCharacters(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    
    for (char c : text) {
        // 保留可打印字符和常用控制字符（换行、回车、制表符）
        if (c >= 32 || c == '\n' || c == '\r' || c == '\t') {
            result += c;
        }
        // 跳过其他控制字符 (0x00-0x1F, 除了 \n, \r, \t)
    }
    
    return result;
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
    
    LOG_DEBUG << "[ClientOutputSanitizer] 正在对 " << clientType << " 客户端的响应进行文本清洗...";
    
    // 1. 基础标签纠错
    fixCommonTagErrors(message);
    
    // 2. 去除非法控制字符
    message = removeControlCharacters(message);
    
    // 注意: 协议转换逻辑（如 XML 标签解析、tool call 提取）已迁移到 ToolCallBridge
    // 这里只负责纯文本清洗
    
    return message;
}
