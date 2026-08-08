#include "sessionManager/core/RequestAdapters.h"
#include <accountManager/accountManager.h>
#include <tools/ZeroWidthEncoder.h>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <sstream>
#include <unordered_set>
#include "sessionManager/core/Session.h"

using namespace drogon;

namespace {

constexpr std::size_t kMaxRequestIdLength = 128;

std::string normalizeIncomingRequestId(const std::string& value)
{
    auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (begin >= end) {
        return "";
    }

    std::string normalized;
    normalized.reserve(std::min<std::size_t>(
        static_cast<std::size_t>(std::distance(begin, end)),
        kMaxRequestIdLength));
    for (auto it = begin; it != end && normalized.size() < kMaxRequestIdLength; ++it) {
        const unsigned char ch = static_cast<unsigned char>(*it);
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ':') {
            normalized.push_back(static_cast<char>(ch));
        } else {
            normalized.push_back('_');
        }
    }
    return normalized;
}

std::string requestIdFromHeaders(const HttpRequestPtr& req)
{
    std::string requestId = normalizeIncomingRequestId(req->getHeader("x-request-id"));
    if (requestId.empty()) {
        requestId = normalizeIncomingRequestId(req->getHeader("x-correlation-id"));
    }
    if (requestId.empty()) {
        requestId = "req_" + drogon::utils::getUuid();
    }
    return requestId;
}

bool assignClientSessionId(Json::Value& clientInfo,
                           const std::string& rawValue,
                           const std::string& source)
{
    const std::string normalized = normalizeIncomingRequestId(rawValue);
    if (normalized.empty()) {
        return false;
    }

    clientInfo["client_session_id"] = normalized;
    clientInfo["client_session_source"] = source;
    return true;
}

void supplementClientSessionIdFromBody(Json::Value& clientInfo,
                                       const Json::Value& requestBody)
{
    if (clientInfo.get("client_type", "").asString() != "Codex" ||
        continuity::hasStableClientSession(clientInfo)) {
        return;
    }

    const auto tryFields = [&clientInfo](const Json::Value& object,
                                         const std::string& prefix) {
        if (!object.isObject()) {
            return false;
        }

        static const char* kFields[] = {
            "thread_id",
            "session_id",
            "conversation_id"
        };
        for (const char* field : kFields) {
            if (object.isMember(field) && object[field].isString() &&
                assignClientSessionId(clientInfo,
                                      object[field].asString(),
                                      prefix + field)) {
                return true;
            }
        }
        return false;
    };

    if (tryFields(requestBody, "body.")) {
        return;
    }
    if (requestBody.isMember("metadata")) {
        tryFields(requestBody["metadata"], "body.metadata.");
    }
}

Json::StreamWriterBuilder& compactJsonWriter()
{
    static thread_local Json::StreamWriterBuilder writer = [] {
        Json::StreamWriterBuilder value;
        value["indentation"] = "";
        return value;
    }();
    return writer;
}

std::string compactJson(const Json::Value& value)
{
    if (value.isString()) return value.asString();
    return Json::writeString(compactJsonWriter(), value);
}

std::string toolNameFromDefinition(const Json::Value& tool)
{
    if (!tool.isObject()) return "";
    if (tool.isMember("function") && tool["function"].isObject()) {
        return tool["function"].get("name", "").asString();
    }
    return tool.get("name", "").asString();
}

std::string appendNamespaceSegment(const std::string& parent,
                                   const std::string& child)
{
    if (parent.empty()) return child;
    if (child.empty()) return parent;
    return parent + "__" + child;
}

std::string bridgeToolName(const std::string& namespacePath,
                           const std::string& originalName)
{
    return appendNamespaceSegment(namespacePath, originalName);
}

Json::Value normalizeToolDefinition(const Json::Value& tool,
                                    const std::string& namespacePath = "")
{
    if (!tool.isObject()) return Json::Value();
    const std::string type = tool.get("type", "").asString();

    if (type == "function") {
        Json::Value normalized(Json::objectValue);
        normalized["type"] = "function";

        Json::Value function(Json::objectValue);
        if (tool.isMember("function") && tool["function"].isObject()) {
            function = tool["function"];
        } else {
            function["name"] = tool.get("name", "").asString();
            function["description"] = tool.get("description", "").asString();
            if (tool.isMember("parameters") && tool["parameters"].isObject()) {
                function["parameters"] = tool["parameters"];
            } else {
                function["parameters"]["type"] = "object";
                function["parameters"]["properties"] = Json::Value(Json::objectValue);
            }
            if (tool.isMember("strict")) function["strict"] = tool["strict"];
        }

        const std::string originalName = function.get("name", "").asString();
        if (originalName.empty()) return Json::Value();

        function["name"] = bridgeToolName(namespacePath, originalName);
        function["_aiapi_original_name"] = originalName;
        if (!namespacePath.empty()) {
            function["_aiapi_namespace"] = namespacePath;
            function["_aiapi_original_type"] = "namespace_function";
        }
        normalized["function"] = std::move(function);
        return normalized;
    }

    if (type == "custom") {
        const std::string originalName = tool.get("name", "").asString();
        if (originalName.empty()) return Json::Value();

        Json::Value normalized(Json::objectValue);
        normalized["type"] = "function";
        auto& function = normalized["function"];
        function["name"] = bridgeToolName(namespacePath, originalName);
        function["description"] = tool.get("description", "").asString();
        function["parameters"]["type"] = "object";
        function["parameters"]["properties"]["input"]["type"] = "string";
        function["parameters"]["required"].append("input");
        function["_aiapi_original_type"] = "custom";
        function["_aiapi_original_name"] = originalName;
        if (!namespacePath.empty()) function["_aiapi_namespace"] = namespacePath;
        return normalized;
    }

    return Json::Value();
}

void appendNormalizedToolDefinition(const Json::Value& tool,
                                    Json::Value& normalizedTools,
                                    std::unordered_set<std::string>& seen,
                                    const std::string& namespacePath,
                                    bool namespaceBridgeEnabled)
{
    if (!tool.isObject()) return;

    const std::string type = tool.get("type", "").asString();
    const std::string name = toolNameFromDefinition(tool);

    if (type == "namespace") {
        if (!namespaceBridgeEnabled) {
            LOG_INFO << "[请求适配器] namespace Tool Bridge 已关闭，跳过工具组: name="
                     << (name.empty() ? "<empty>" : name);
            return;
        }
        const std::string childNamespace = appendNamespaceSegment(namespacePath, name);
        if (name.empty() || !tool.isMember("tools") || !tool["tools"].isArray()) {
            LOG_WARN << "[请求适配器] namespace 工具组无有效名称或 tools 数组: name="
                     << (name.empty() ? "<empty>" : name);
            return;
        }

        LOG_INFO << "[请求适配器] 展开 namespace 工具组: namespace="
                  << childNamespace << ", nested=" << tool["tools"].size();
        for (const auto& child : tool["tools"]) {
            appendNormalizedToolDefinition(
                child, normalizedTools, seen, childNamespace, namespaceBridgeEnabled);
        }
        return;
    }

    Json::Value normalized = normalizeToolDefinition(tool, namespacePath);
    if (normalized.isNull()) {
        LOG_DEBUG << "[请求适配器] 工具定义无法桥接，已保留原始定义但不会注入 XML bridge: type="
                 << (type.empty() ? "<empty>" : type)
                 << ", name=" << (name.empty() ? "<empty>" : name)
                 << ", namespace=" << (namespacePath.empty() ? "<none>" : namespacePath);
        return;
    }

    const std::string normalizedName = toolNameFromDefinition(normalized);
    const std::string key = "function:" + normalizedName;
    if (!normalizedName.empty() && !seen.insert(key).second) return;
    normalizedTools.append(std::move(normalized));
}

void appendToolDefinitions(const Json::Value& tools,
                           Json::Value& rawTools,
                           Json::Value& normalizedTools,
                           std::unordered_set<std::string>& seen,
                           bool namespaceBridgeEnabled)
{
    if (!tools.isArray()) return;
    for (const auto& tool : tools) {
        if (!tool.isObject()) continue;

        // toolsRaw 保留客户端发送的顶层结构；namespace 子项不再重复追加。
        rawTools.append(tool);
        appendNormalizedToolDefinition(
            tool, normalizedTools, seen, "", namespaceBridgeEnabled);
    }
}

void collectAdditionalTools(const Json::Value& items,
                            Json::Value& rawTools,
                            Json::Value& normalizedTools,
                            std::unordered_set<std::string>& seen,
                            bool namespaceBridgeEnabled)
{
    if (!items.isArray()) return;
    for (const auto& item : items) {
        if (item.isObject() && item.get("type", "").asString() == "additional_tools") {
            appendToolDefinitions(
                item["tools"], rawTools, normalizedTools, seen, namespaceBridgeEnabled);
        }
    }
}

bool isCodexToolOutputType(const std::string& type)
{
    return type == "function_call_output" || type == "tool_call_output" ||
           type == "custom_tool_call_output" || type == "mcp_tool_call_output" ||
           type == "local_shell_call_output" || type == "tool_search_output";
}

bool isCodexToolCallType(const std::string& type)
{
    return type == "function_call" || type == "tool_call" ||
           type == "custom_tool_call" || type == "mcp_tool_call" ||
           type == "local_shell_call" || type == "tool_search_call";
}

std::string codexToolOutputText(const Json::Value& item)
{
    const std::string type = item.get("type", "tool_call_output").asString();
    const std::string callId = item.get("call_id", "").asString();
    const std::string outputStr = compactJson(item.get("output", Json::Value("")));

    // 观测点：区分"客户端未回传完整工具结果"与"桥接层丢内容"。
    LOG_DEBUG << "[请求适配器] 工具结果原文: type=" << type
              << ", callId=" << (callId.empty() ? "(空)" : callId)
              << ", outputBytes=" << outputStr.size()
              << ", head=" << outputStr.substr(0, std::min<size_t>(outputStr.size(), 200));

    std::string text = "\n[tool_result type=" + type;
    if (!callId.empty()) text += " call_id=" + callId;
    text += "]\n" + outputStr + "\n[/tool_result]\n";
    return text;
}

void appendCodexToolOutput(const Json::Value& item, std::string& currentInput)
{
    currentInput += codexToolOutputText(item);
}

Message makeCodexToolOutputHistory(const Json::Value& item)
{
    Message message;
    message.role = MessageRole::Tool;
    message.toolCallId = item.get("call_id", "").asString();

    ContentPart part;
    part.type = ContentPartType::Text;
    part.text = codexToolOutputText(item);
    message.content.push_back(std::move(part));
    return message;
}

Message makeCodexToolCallHistory(const Json::Value& item)
{
    const std::string type = item.get("type", "tool_call").asString();
    const std::string callId = item.get("call_id", item.get("id", "")).asString();
    const std::string name = item.get("name", "").asString();
    const std::string namespacePath = item.get("namespace", "").asString();
    const Json::Value payload = item.isMember("arguments")
        ? item["arguments"] : item.get("input", Json::Value(""));
    Message message = Message::assistant("");
    Json::Value toolCall(Json::objectValue);
    toolCall["id"] = callId;
    toolCall["type"] = type == "custom_tool_call" ? "custom" : "function";
    Json::Value function(Json::objectValue);
    function["name"] = bridgeToolName(namespacePath, name);
    if (!namespacePath.empty()) {
        function["_aiapi_namespace"] = namespacePath;
        function["_aiapi_original_name"] = name;
    }
    function["arguments"] = payload.isString() ? payload.asString() : compactJson(payload);
    toolCall["function"] = std::move(function);
    message.toolCalls.push_back(std::move(toolCall));
    return message;
}

bool isResponsesModelOutputBoundary(const Json::Value& item)
{
    if (!item.isObject()) return false;
    if (item.get("role", "").asString() == "assistant") return true;
    return isCodexToolCallType(item.get("type", "").asString());
}

}  // namespace

GenerationRequest RequestAdapters::buildGenerationRequestFromChat(
    const HttpRequestPtr& req
) {
    LOG_INFO << "[请求适配器] 从 API 请求构建 Generation请求";
    
    GenerationRequest genReq;
    genReq.endpointType = EndpointType::ChatCompletions;
    genReq.requestId = requestIdFromHeaders(req);
    
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        LOG_WARN << "[请求适配器] API 请求体无效";
        return genReq;
    }
    
    const Json::Value& reqBody = *jsonPtr;
    
    // 1. 提取基本参数
    genReq.model = reqBody.get("model", "").asString();
    //一些模型映射
    if(genReq.model=="CAI-CL045")
    {
        genReq.model="Claude Opus 4.5";
    }
    genReq.stream = reqBody.get("stream", false).asBool();
    genReq.provider = "chaynsapi";  // 默认 上游
    

    if (reqBody.isMember("tools") && reqBody["tools"].isArray()) {
        genReq.tools = reqBody["tools"];
        genReq.toolsRaw = reqBody["tools"];
        LOG_INFO << "[请求适配器] API 请求包含" << genReq.tools.size() << " 个工具定义";
    }
    if (reqBody.isMember("parallel_tool_calls") && reqBody["parallel_tool_calls"].isBool()) {
        genReq.parallelToolCalls = reqBody["parallel_tool_calls"].asBool();
    }
    

    if (reqBody.isMember("tool_choice")) {
        if (reqBody["tool_choice"].isString()) {
            genReq.toolChoice = reqBody["tool_choice"].asString();
        } else if (reqBody["tool_choice"].isObject()) {
            //？
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            genReq.toolChoice = Json::writeString(writer, reqBody["tool_choice"]);
        }
    }
    
    // 2. 提取客户端信息
    genReq.clientInfo = extractClientInfo(req);
    if (reqBody.isMember("workspaceId") && reqBody["workspaceId"].isString()) {
        genReq.clientInfo["workspace_id"] = reqBody["workspaceId"].asString();
    } else if (reqBody.isMember("workspace_id") && reqBody["workspace_id"].isString()) {
        genReq.clientInfo["workspace_id"] = reqBody["workspace_id"].asString();
    }
    supplementClientSessionIdFromBody(genReq.clientInfo, reqBody);
    

    std::vector<ImageInfo> images;
    if (reqBody.isMember("messages") && reqBody["messages"].isArray()) {
        // continuityTexts： 保留原始文本（包含零宽字符），用于后续会话连续性解析
        // 注意：解析ChatMessages 内部会按需 stripZeroWidth，避免把零宽数据传给上游模型
        genReq.continuityTexts.clear();
        for (const auto& m : reqBody["messages"]) {
            if (!m.isObject()) continue;
            if (!m.isMember("content")) continue;
            const auto& c = m["content"];
            if (c.isString()) {
                genReq.continuityTexts.push_back(c.asString());
            } else if (c.isArray()) {
                for (const auto& item : c) {
                    if (!item.isObject()) continue;
                    const std::string type = item.get("type", "").asString();
                    if ((type == "text" || type == "input_text" || type == "output_text") &&
                        item.isMember("text") && item["text"].isString()) {
                        genReq.continuityTexts.push_back(item["text"].asString());
                    } else if (item.isMember("text") && item["text"].isString()) {
                        genReq.continuityTexts.push_back(item["text"].asString());
                    }
                }
            }
        }

        parseChatMessages(
            reqBody["messages"],
            genReq.messages,
            genReq.systemPrompt,
            genReq.currentInput,
            images
        );
    }
    
    // 将提取的图片传递给 Generation请求
    genReq.images = images;
    
    LOG_INFO << "[请求适配器] API 请求解析完成，模型：" << genReq.model
             << ", messages: " << genReq.messages.size()
             << ", currentInput length: " << genReq.currentInput.length()
             << ", images: " << images.size();
    
    return genReq;
}

GenerationRequest RequestAdapters::buildGenerationRequestFromResponses(
    const HttpRequestPtr& req
) {
    LOG_INFO << "[请求适配器] 从 Responses API 请求构建 Generation请求";
    
    GenerationRequest genReq;
    genReq.endpointType = EndpointType::Responses;
    genReq.requestId = requestIdFromHeaders(req);
    
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        LOG_WARN << "[请求适配器] Responses API 请求体无效";
        return genReq;
    }
    
    const Json::Value& reqBody = *jsonPtr;
    
    // 1. 提取基本参数
    genReq.model = reqBody.get("model", "GPT-4o").asString();
    genReq.stream = reqBody.get("stream", false).asBool();
    genReq.systemPrompt = reqBody.get("instructions", "").asString();
    genReq.provider = "chaynsapi";  // 默认 上游
    

    Json::Value rawTools(Json::arrayValue);
    Json::Value normalizedTools(Json::arrayValue);
    std::unordered_set<std::string> seenTools;
    const bool namespaceBridgeEnabled =
        AccountManager::getInstance().getAccountAutomationSettings().namespaceToolBridgeEnabled;
    appendToolDefinitions(
        reqBody["tools"], rawTools, normalizedTools, seenTools, namespaceBridgeEnabled);
    collectAdditionalTools(
        reqBody["input"], rawTools, normalizedTools, seenTools, namespaceBridgeEnabled);
    collectAdditionalTools(
        reqBody["input_items"], rawTools, normalizedTools, seenTools, namespaceBridgeEnabled);
    genReq.toolsRaw = std::move(rawTools);
    genReq.tools = std::move(normalizedTools);
    if (!genReq.toolsRaw.empty()) {
        LOG_INFO << "[请求适配器] Responses API 工具定义：原始=" << genReq.toolsRaw.size()
                 << ", 可桥接=" << genReq.tools.size();
        //LOG_INFO << "[请求适配器] Responses API 工具定义：原始=" << genReq.toolsRaw.toStyledString();
        //LOG_INFO << "[请求适配器] Responses API 工具定义：桥接后=" << genReq.tools.toStyledString();
    }
    if (reqBody.isMember("parallel_tool_calls") && reqBody["parallel_tool_calls"].isBool()) {
        genReq.parallelToolCalls = reqBody["parallel_tool_calls"].asBool();
    }
    

    if (reqBody.isMember("tool_choice")) {
        if (reqBody["tool_choice"].isString()) {
            genReq.toolChoice = reqBody["tool_choice"].asString();
        } else if (reqBody["tool_choice"].isObject()) {
            Json::Value choice = reqBody["tool_choice"];
            const std::string choiceType = choice.get("type", "").asString();
            if ((choiceType == "function" || choiceType == "custom") &&
                choice.isMember("name") && choice["name"].isString() &&
                !choice.isMember("function")) {
                Json::Value nested(Json::objectValue);
                nested["type"] = "function";
                nested["function"]["name"] = choice["name"];
                choice = std::move(nested);
            }
            genReq.toolChoice = Json::writeString(compactJsonWriter(), choice);
        }
    }
    LOG_INFO << "[请求适配器] Responses API 工具策略: tool_choice="
             << (genReq.toolChoice.empty() ? "auto(default)" : genReq.toolChoice)
             << ", parallel_tool_calls=" << genReq.parallelToolCalls;
    
    // 2. 提取客户端信息
    genReq.clientInfo = extractClientInfo(req);
    if (reqBody.isMember("workspaceId") && reqBody["workspaceId"].isString()) {
        genReq.clientInfo["workspace_id"] = reqBody["workspaceId"].asString();
    } else if (reqBody.isMember("workspace_id") && reqBody["workspace_id"].isString()) {
        genReq.clientInfo["workspace_id"] = reqBody["workspace_id"].asString();
    }
    supplementClientSessionIdFromBody(genReq.clientInfo, reqBody);
    
    // 3. 处理 previous_响应_id（用于续聊）
    if (reqBody.isMember("previous_response_id") &&
        reqBody["previous_response_id"].isString() &&
        !reqBody["previous_response_id"].asString().empty()) {
        genReq.previousResponseId = reqBody["previous_response_id"].asString();
        LOG_INFO << "[请求适配器] 检测到 previous_响应_id：" << *genReq.previousResponseId;
    }
    
    // 4. continuityTexts：覆盖 //input_items 三个来源（保留原始文本，包含零宽字符）
    genReq.continuityTexts.clear();
    auto collectTextFromValue = [&genReq](const Json::Value& v, auto&& self) -> void {
        if (v.isString()) {
            genReq.continuityTexts.push_back(v.asString());
            return;
        }
        if (v.isArray()) {
            for (const auto& it : v) self(it, self);
            return;
        }
        if (!v.isObject()) return;
        if (v.get("type", "").asString() == "additional_tools") return;

        if (v.isMember("text") && v["text"].isString()) {
            genReq.continuityTexts.push_back(v["text"].asString());
        }

        if (v.isMember("content")) {
            const auto& c = v["content"];
            if (c.isString()) {
                genReq.continuityTexts.push_back(c.asString());
            } else if (c.isArray()) {
                for (const auto& item : c) {
                    if (!item.isObject()) continue;
                    const std::string type = item.get("type", "").asString();
                    if ((type == "text" || type == "input_text" || type == "output_text") &&
                        item.isMember("text") && item["text"].isString()) {
                        genReq.continuityTexts.push_back(item["text"].asString());
                    } else if (item.isMember("text") && item["text"].isString()) {
                        genReq.continuityTexts.push_back(item["text"].asString());
                    }
                }
            }
        }
    };

    if (reqBody.isMember("input")) {
        collectTextFromValue(reqBody["input"], collectTextFromValue);
    }
    if (reqBody.isMember("messages")) {
        collectTextFromValue(reqBody["messages"], collectTextFromValue);
    }
    if (reqBody.isMember("input_items")) {
        collectTextFromValue(reqBody["input_items"], collectTextFromValue);
    }

    // 5. 解析输入（按优先级： > > input_items）
    std::vector<ImageInfo> images;
    if (reqBody.isMember("input")) {
        parseResponseInput(
            reqBody["input"],
            genReq.messages,
            genReq.systemPrompt,
            genReq.currentInput,
            images
        );
    } else if (reqBody.isMember("messages") && reqBody["messages"].isArray()) {
        parseChatMessages(
            reqBody["messages"],
            genReq.messages,
            genReq.systemPrompt,
            genReq.currentInput,
            images
        );
    } else if (reqBody.isMember("input_items") && reqBody["input_items"].isArray()) {
        parseResponseInputItems(reqBody["input_items"], genReq.currentInput, images);
    }
    
    // 将提取的图片传递给 Generation请求
    genReq.images = images;
    
    LOG_INFO << "[请求适配器] Responses API 请求解析完成，模型：" << genReq.model
             << ", messages: " << genReq.messages.size()
             << ", currentInput length: " << genReq.currentInput.length()
             << ", images: " << images.size();
    
    return genReq;
}

Json::Value RequestAdapters::extractClientInfo(const HttpRequestPtr& req) {
    Json::Value clientInfo;
    
    // 提取客户端标识。User-Agent 只能用于协议适配，不能用于鉴权。
    const std::string userAgent = req->getHeader("user-agent");
    const std::string originator = req->getHeader("originator");
    const std::string codexWindowId = req->getHeader("x-codex-window-id");
    // Only enable the Codex protocol adapter for an explicitly identified
    // Codex client. x-codex-window-id is also sent by some Codex-compatible
    // clients and is therefore insufficient evidence of native Responses SSE
    // and function-call support on its own.
    const bool isCodex = originator == "codex-tui" ||
                         userAgent.rfind("codex-tui/", 0) == 0 ||
                         userAgent.rfind("codex_cli_rs/", 0) == 0;
    std::string clientType = userAgent;

    if (isCodex) {
        clientType = "Codex";
        clientInfo["client_variant"] = "codex-tui";
        if (!assignClientSessionId(clientInfo, req->getHeader("thread-id"), "header.thread-id") &&
            !assignClientSessionId(clientInfo, req->getHeader("session-id"), "header.session-id") &&
            !assignClientSessionId(clientInfo, req->getHeader("session_id"), "header.session_id") &&
            !assignClientSessionId(clientInfo, req->getHeader("conversation-id"), "header.conversation-id")) {
            assignClientSessionId(clientInfo,
                                  req->getHeader("conversation_id"),
                                  "header.conversation_id");
        }
    } else if (userAgent.find("Kilo-Code") != std::string::npos) {
        clientType = "Kilo-Code";
    } else if (userAgent.find("RooCode") != std::string::npos) {
        clientType = "RooCode";
    }

    clientInfo["client_type"] = clientType;
    if (!userAgent.empty()) {
        clientInfo["raw_user_agent"] = userAgent;
    }
    

    std::string auth = req->getHeader("authorization");
    if (auth.empty()) {
        auth = req->getHeader("Authorization");
    }
    

    auto stripBearer = [](std::string& s) {
        const std::string p1 = "Bearer ";
        const std::string p2 = "bearer ";
        if (s.rfind(p1, 0) == 0) s = s.substr(p1.size());
        else if (s.rfind(p2, 0) == 0) s = s.substr(p2.size());

        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    };
    stripBearer(auth);
    clientInfo["client_authorization"] = auth;
    
    LOG_INFO << "[请求适配器] 识别到客户端类型：" << (clientType.empty() ? "未知" : clientType)
             << ", rawUserAgent=" << (userAgent.empty() ? "(空)" : userAgent)
             << ", originator=" << (originator.empty() ? "(空)" : originator)
             << ", codexWindowId=" << (codexWindowId.empty() ? "(空)" : codexWindowId);
    LOG_INFO << "[请求适配器] 客户端凭证："
             << (auth.empty() ? "未提供" : ("已提供(长度=" + std::to_string(auth.size()) + ")"));
    
    return clientInfo;
}

void RequestAdapters::parseChatMessages(
    const Json::Value& messages,
    std::vector<Message>& result,
    std::string& systemPrompt,
    std::string& currentInput,
    std::vector<ImageInfo>& images
) {
    if (!messages.isArray() || messages.empty()) {
        return;
    }
    
    // 查找最后一个 消息的索引（用于分割历史和当前输入）
    int splitIndex = -1;
    for (int i = static_cast<int>(messages.size()) - 1; i > 0; i--) {
        if (messages[i]["role"].asString() == "assistant") {
            splitIndex = i;
            break;
        }
    }
    
    // 检查是否使用零宽字符模式
    bool isZeroWidthMode = chatSession::getInstance()->isZeroWidthMode();
    
    // 用于临时存储历史消息中的图片（不加入当前请求图片）
    std::vector<ImageInfo> tempImages;
    
    for (int i = 0; i < static_cast<int>(messages.size()); i++) {
        const Json::Value& msg = messages[i];
        std::string role = msg.get("role", "user").asString();
        

        if (role == "system") {
            systemPrompt += extractContentText(msg["content"], tempImages, isZeroWidthMode);
            continue;
        }
        
        // 分割历史消息和当前输入
        if (i <= splitIndex) {
            // 历史消息：添加到 列表
            Message message;
            if (role == "user") {
                message.role = MessageRole::User;
            } else if (role == "assistant") {
                message.role = MessageRole::Assistant;
            } else if (role == "tool") {
                message.role = MessageRole::Tool;

                if (msg.isMember("tool_call_id")) {
                    message.toolCallId = msg["tool_call_id"].asString();
                }
            } else {
                continue;  // 跳过未知角色
            }
            
            std::string text = extractContentText(msg["content"], tempImages, isZeroWidthMode);
            ContentPart part;
            part.type = ContentPartType::Text;
            part.text = text;
            message.content.push_back(part);
            
            // 提取 消息中的 tool_calls
            if (role == "assistant" && msg.isMember("tool_calls") && msg["tool_calls"].isArray()) {
                for (const auto& tc : msg["tool_calls"]) {
                    message.toolCalls.push_back(tc);
                }
            }
            
            // 合并连续的 user 消息（保持工具调用边界不变）
            if (!result.empty() &&
                message.role == MessageRole::User &&
                result.back().role == MessageRole::User &&
                message.toolCalls.empty() && result.back().toolCalls.empty()) {
                result.back().content[0].text += text;
            } else {
                result.push_back(message);
            }
        } else {
            // 当前输入：
            // - 标准 ChatCompletions： 最后由 消息作为当前输入
            // - Kilo-Code 等 客户端：可能以 = 发送最新的 结果/
            // 作为本轮模型需要处理的“当前输入”，因此这里也要接收 消息。
            if (role == "user" || role == "tool") {
                currentInput += extractContentText(msg["content"], images, isZeroWidthMode);
            }
        }
    }
}

void RequestAdapters::parseResponseInput(
    const Json::Value& input,
    std::vector<Message>& messages,
    std::string& systemPrompt,
    std::string& currentInput,
    std::vector<ImageInfo>& images
) {
    // 检查是否使用零宽字符模式
    const bool isZeroWidthMode = chatSession::getInstance()->isZeroWidthMode();

    if (input.isString()) {
        // 简单字符串输入
        currentInput = input.asString();
        if (isZeroWidthMode) {
            currentInput = ZeroWidthEncoder::stripZeroWidth(currentInput);
        }
        return;
    }

    if (!input.isArray()) {
        return;
    }

    // Responses 请求可能携带完整客户端转录。chayns 上游不具备原生
    // 工具调用：我们只能把最近一次模型输出之后的用户/工具结果作为
    // 本轮 XML bridge 后续消息。更早的工具结果属于历史，不能每轮
    // 重复拼入 currentInput。
    //
    // 边界不能只看 role=assistant：Codex Responses 在模型请求工具时
    // 可能只有 function_call/custom_tool_call 等模型输出项。
    int lastModelOutputIndex = -1;
    for (int i = static_cast<int>(input.size()) - 1; i >= 0; --i) {
        if (isResponsesModelOutputBoundary(input[i])) {
            lastModelOutputIndex = i;
            break;
        }
    }
    const int currentInputStart = lastModelOutputIndex + 1;

    // 历史图片不应重复上传到本轮。
    std::vector<ImageInfo> historyImages;
    size_t currentToolOutputCount = 0;
    size_t currentToolOutputBytes = 0;
    size_t historicalToolOutputCount = 0;
    size_t historicalToolOutputBytes = 0;

    auto appendTextByPosition = [&](int index, const std::string& text) {
        if (text.empty()) return;
        if (index >= currentInputStart) {
            currentInput += text;
        } else {
            messages.push_back(Message::user(text));
        }
    };

    for (int i = 0; i < static_cast<int>(input.size()); ++i) {
        const auto& item = input[i];

        if (item.isString()) {
            std::string text = item.asString();
            if (isZeroWidthMode) {
                text = ZeroWidthEncoder::stripZeroWidth(text);
            }
            appendTextByPosition(i, text + "\n");
            continue;
        }

        if (!item.isObject()) {
            continue;
        }

        const std::string type = item.get("type", "").asString();
        const std::string role = item.get("role", "").asString();

        if (type == "additional_tools") {
            continue;
        }

        if (isCodexToolOutputType(type)) {
            const std::string toolResult = codexToolOutputText(item);
            if (i >= currentInputStart) {
                currentInput += toolResult;
                ++currentToolOutputCount;
                currentToolOutputBytes += toolResult.size();
            } else {
                messages.push_back(makeCodexToolOutputHistory(item));
                ++historicalToolOutputCount;
                historicalToolOutputBytes += toolResult.size();
            }
            continue;
        }

        if (isCodexToolCallType(type)) {
            // 工具调用项是上一次 XML bridge 模型输出的客户端表示。
            // 对已有 chayns 线程它已存在于上游上下文；对新线程重放
            // 则必须留在历史中。它本身不应再发成本轮用户文本。
            messages.push_back(makeCodexToolCallHistory(item));
            continue;
        }

        if (!role.empty()) {
            // Responses API 的 developer/system 消息是本轮指令，不应混入用户输入。
            if (role == "system" || role == "developer") {
                if (item.isMember("content")) {
                    std::string instruction = extractContentText(
                        item["content"], historyImages, isZeroWidthMode);
                    if (!instruction.empty()) {
                        if (!systemPrompt.empty() && systemPrompt.back() != '\n') {
                            systemPrompt += '\n';
                        }
                        systemPrompt += instruction;
                    }
                }
                continue;
            }

            if (i < currentInputStart) {
                Message message;
                if (role == "user") {
                    message.role = MessageRole::User;
                } else if (role == "assistant") {
                    message.role = MessageRole::Assistant;
                } else if (role == "tool") {
                    message.role = MessageRole::Tool;
                    message.toolCallId = item.get("tool_call_id", "").asString();
                } else {
                    continue;
                }

                std::string text;
                if (item.isMember("content")) {
                    text = extractContentText(
                        item["content"], historyImages, isZeroWidthMode);
                }
                ContentPart part;
                part.type = ContentPartType::Text;
                part.text = text;
                message.content.push_back(std::move(part));
                messages.push_back(std::move(message));
            } else if (role == "user" || role == "tool") {
                // chayns 上游只能看到纯文本，所以将本轮标准 tool 角色内容
                // 与 Codex *_call_output 一样作为后续消息。
                if (item.isMember("content")) {
                    currentInput += extractContentText(
                        item["content"], images, isZeroWidthMode);
                }
            }
            continue;
        }

        if (type == "input_text" || type == "text") {
            std::string text = item.get("text", "").asString();
            if (isZeroWidthMode) {
                text = ZeroWidthEncoder::stripZeroWidth(text);
            }
            appendTextByPosition(i, text);
            continue;
        }

        if (type == "input_image") {
            // 历史图片不重复上传。
            if (i < currentInputStart) continue;

            std::string url;
            if (item.isMember("image_url")) {
                url = item["image_url"].asString();
            } else if (item.isMember("url")) {
                url = item["url"].asString();
            } else if (item.isMember("file") && item["file"].isObject()) {
                const auto& fileObj = item["file"];
                if (fileObj.isMember("url")) {
                    url = fileObj["url"].asString();
                }
            }

            if (!url.empty()) {
                ImageInfo imgInfo = parseImageUrl(url);
                if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                    images.push_back(imgInfo);
                    LOG_INFO << "[请求适配器] 提取到图片(input_image)，mediaType："
                             << imgInfo.mediaType;
                }
            }
            continue;
        }

        if (type == "image_url") {
            if (i >= currentInputStart &&
                item.isMember("image_url") && item["image_url"].isObject()) {
                const auto& imageUrl = item["image_url"];
                if (imageUrl.isMember("url")) {
                    ImageInfo imgInfo = parseImageUrl(imageUrl["url"].asString());
                    if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                        images.push_back(imgInfo);
                    }
                }
            }
            continue;
        }

        // 兼容无 role 的 message/content 项。
        if (type == "message" && item.isMember("content")) {
            std::vector<ImageInfo>& targetImages =
                i >= currentInputStart ? images : historyImages;
            const std::string text = extractContentText(
                item["content"], targetImages, isZeroWidthMode);
            appendTextByPosition(i, text);
            continue;
        }

        if (item.isMember("text") && item["text"].isString()) {
            std::string text = item["text"].asString();
            if (isZeroWidthMode) {
                text = ZeroWidthEncoder::stripZeroWidth(text);
            }
            appendTextByPosition(i, text);
        }
    }

    if (currentToolOutputCount > 0 || historicalToolOutputCount > 0) {
        LOG_INFO << "[请求适配器] Responses 工具结果分轮: currentCount="
                 << currentToolOutputCount
                 << ", currentBytes=" << currentToolOutputBytes
                 << ", historicalCount=" << historicalToolOutputCount
                 << ", historicalBytes=" << historicalToolOutputBytes
                 << ", lastModelOutputIndex=" << lastModelOutputIndex;
    }
}

void RequestAdapters::parseResponseInputItems(
    const Json::Value& inputItems,
    std::string& currentInput,
    std::vector<ImageInfo>& images
) {
    if (!inputItems.isArray()) return;

    const bool isZeroWidthMode = chatSession::getInstance()->isZeroWidthMode();

    // input_items 也可能是完整转录的退化形式。它没有 messages 输出参数可供
    // 重放历史，因此只保留最后一次模型输出后的增量。
    int lastModelOutputIndex = -1;
    for (int i = static_cast<int>(inputItems.size()) - 1; i >= 0; --i) {
        if (isResponsesModelOutputBoundary(inputItems[i])) {
            lastModelOutputIndex = i;
            break;
        }
    }
    const int currentInputStart = lastModelOutputIndex + 1;

    for (int index = 0; index < static_cast<int>(inputItems.size()); ++index) {
        const auto& item = inputItems[index];
        if (item.isString()) {
            if (index < currentInputStart) continue;
            std::string text = item.asString();
            if (isZeroWidthMode) {
                text = ZeroWidthEncoder::stripZeroWidth(text);
            }
            currentInput += text;
            if (!text.empty() && text.back() != '\n') {
                currentInput += "\n";
            }
            continue;
        }

        if (!item.isObject()) continue;

        const std::string type = item.get("type", "").asString();

        if (type == "additional_tools") continue;
        if (isCodexToolOutputType(type)) {
            if (index < currentInputStart) continue;
            appendCodexToolOutput(item, currentInput);
            continue;
        }
        if (isCodexToolCallType(type)) {
            // 模型输出本身已在 chayns 线程中，不应再次作为当前用户输入。
            if (index >= currentInputStart) {
                currentInput += "\n" + makeCodexToolCallHistory(item).getTextContent() + "\n";
            }
            continue;
        }

        if ((type == "input_text" || type == "text") &&
            item.isMember("text") && item["text"].isString()) {
            if (index < currentInputStart) continue;
            std::string text = item["text"].asString();
            if (isZeroWidthMode) {
                text = ZeroWidthEncoder::stripZeroWidth(text);
            }
            currentInput += text;
            if (!text.empty() && text.back() != '\n') {
                currentInput += "\n";
            }
            continue;
        }


        if (type == "message" && item.isMember("content")) {
            if (index < currentInputStart) continue;
            currentInput += extractContentText(item["content"], images, isZeroWidthMode);
            continue;
        }

        // 图片输入
        if (type == "input_image") {
            if (index < currentInputStart) continue;
            std::string url;
            if (item.isMember("image_url")) {
                url = item["image_url"].asString();
            } else if (item.isMember("url")) {
                url = item["url"].asString();
            } else if (item.isMember("file") && item["file"].isObject()) {
                const auto& fileObj = item["file"];
                if (fileObj.isMember("url")) {
                    url = fileObj["url"].asString();
                }
            }

            if (!url.empty()) {
                ImageInfo imgInfo = parseImageUrl(url);
                if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                    images.push_back(imgInfo);
                }
            }
            continue;
        }


        if (type == "image_url") {
            if (index < currentInputStart) continue;
            if (item.isMember("image_url") && item["image_url"].isObject()) {
                const auto& imageUrl = item["image_url"];
                if (imageUrl.isMember("url")) {
                    ImageInfo imgInfo = parseImageUrl(imageUrl["url"].asString());
                    if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                        images.push_back(imgInfo);
                    }
                }
            }
            continue;
        }


        if (item.isMember("text") && item["text"].isString()) {
            if (index < currentInputStart) continue;
            std::string text = item["text"].asString();
            if (isZeroWidthMode) {
                text = ZeroWidthEncoder::stripZeroWidth(text);
            }
            currentInput += text;
            if (!text.empty() && text.back() != '\n') {
                currentInput += "\n";
            }
        }
    }
}

std::string RequestAdapters::extractContentText(
    const Json::Value& content,
    std::vector<ImageInfo>& images,
    bool stripZeroWidth,
    std::vector<std::string>* outRawTexts
) {
    if (content.isString()) {
        std::string text = content.asString();
        if (outRawTexts) {
            outRawTexts->push_back(text);
        }
        if (stripZeroWidth) {
            text = ZeroWidthEncoder::stripZeroWidth(text);
        }
        return text;
    }
    
    if (!content.isArray()) {
        return "";
    }
    
    std::string result;
    for (const auto& item : content) {
        if (!item.isObject()) {
            continue;
        }
        
        std::string itemType = item.get("type", "").asString();
        
        // 处理文本类型：支持 "" ( API) 和 "input_text" (响应 API)
        if ((itemType == "text" || itemType == "input_text" || itemType == "output_text") && 
            item.isMember("text") && item["text"].isString()) {
            std::string textPart = item["text"].asString();
            if (outRawTexts) {
                outRawTexts->push_back(textPart);
            }
            if (stripZeroWidth) {
                textPart = ZeroWidthEncoder::stripZeroWidth(textPart);
            }
            result += textPart;
            if (!textPart.empty() && textPart.back() != '\n') {
                result += "\n";
            }
        }
        // 处理图片类型：image_url ( API)
        else if (itemType == "image_url") {
            if (item.isMember("image_url") && item["image_url"].isObject()) {
                const auto& imageUrl = item["image_url"];
                if (imageUrl.isMember("url") && imageUrl["url"].isString()) {
                    ImageInfo imgInfo = parseImageUrl(imageUrl["url"].asString());
                    if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                        images.push_back(imgInfo);
                    }
                }
            }
        }
        // 处理图片类型：input_image (响应 API)
        else if (itemType == "input_image") {
            std::string url;
            if (item.isMember("image_url") && item["image_url"].isString()) {
                url = item["image_url"].asString();
            } else if (item.isMember("url") && item["url"].isString()) {
                url = item["url"].asString();
            } else if (item.isMember("file") && item["file"].isObject()) {
                const auto& fileObj = item["file"];
                if (fileObj.isMember("url") && fileObj["url"].isString()) {
                    url = fileObj["url"].asString();
                }
            }
            
            if (!url.empty()) {
                ImageInfo imgInfo = parseImageUrl(url);
                if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                    images.push_back(imgInfo);
                }
            }
        }
    }
    
    return result;
}

ImageInfo RequestAdapters::parseImageUrl(const std::string& url) {
    ImageInfo imgInfo;
    
    if (url.empty()) {
        return imgInfo;
    }
    
    // 检查是否是 编码的图片
    if (url.find("data:") == 0) {

        size_t semicolonPos = url.find(";");
        size_t commaPos = url.find(",");
        if (semicolonPos != std::string::npos && commaPos != std::string::npos) {
            imgInfo.mediaType = url.substr(5, semicolonPos - 5);  // 提取 image/png
            imgInfo.base64Data = url.substr(commaPos + 1);        // 提取 base64 数据
        }
    } else {

        imgInfo.uploadedUrl = url;
    }
    
    return imgInfo;
}