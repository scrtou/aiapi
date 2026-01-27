#include "XmlTagToolCallCodec.h"
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>
#include <drogon/utils/Utilities.h>

namespace toolcall {

// XML标签常量
static const std::string TAG_FUNCTION_CALLS = "antml:function_calls";
static const std::string TAG_FUNCTION_CALLS_PLAIN = "function_calls";
static const std::string TAG_INVOKE = "antml:invoke";
static const std::string TAG_INVOKE_PLAIN = "invoke";
static const std::string TAG_PARAMETER = "antml:parameter";
static const std::string TAG_PARAMETER_PLAIN = "parameter";
static const std::string TAG_FUNCTION_CALL = "function_call";
static const std::string TAG_TOOL = "tool";
static const std::string TAG_ARGS_JSON = "args_json";
static const std::string TAG_TOOL_RESULT = "tool_result";

// 哨兵标记
static const std::string SENTINEL_START = "<Function_o2gx_Start/>";

XmlTagToolCallCodec::XmlTagToolCallCodec()
    : state_(XmlParserState::Text)
    , currentParamEndTag_()
    , toolCallCounter_(0) {
}

std::string XmlTagToolCallCodec::generateToolCallId() {
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

std::string XmlTagToolCallCodec::escapeXml(const std::string& text) {
    std::string result;
    result.reserve(text.size() * 1.1);
    
    for (char c : text) {
        switch (c) {
            case '<': result += "&"; result += "lt;"; break;
            case '>': result += "&"; result += "gt;"; break;
            case '&': result += "&"; result += "amp;"; break;
            case '"': result += "&"; result += "quot;"; break;
            case '\'': result += "&"; result += "apos;"; break;
            default: result += c; break;
        }
    }
    return result;
}

std::string XmlTagToolCallCodec::unescapeXml(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '&') {
            // Check for <
            if (i + 3 < text.size() && text[i+1] == 'l' && text[i+2] == 't' && text[i+3] == ';') {
                result += '<';
                i += 4;
            // Check for >
            } else if (i + 3 < text.size() && text[i+1] == 'g' && text[i+2] == 't' && text[i+3] == ';') {
                result += '>';
                i += 4;
            // Check for &
            } else if (i + 4 < text.size() && text[i+1] == 'a' && text[i+2] == 'm' && text[i+3] == 'p' && text[i+4] == ';') {
                result += '&';
                i += 5;
            // Check for "
            } else if (i + 5 < text.size() && text[i+1] == 'q' && text[i+2] == 'u' && text[i+3] == 'o' && text[i+4] == 't' && text[i+5] == ';') {
                result += '"';
                i += 6;
            // Check for '
            } else if (i + 5 < text.size() && text[i+1] == 'a' && text[i+2] == 'p' && text[i+3] == 'o' && text[i+4] == 's' && text[i+5] == ';') {
                result += '\'';
                i += 6;
            } else {
                result += text[i++];
            }
        } else {
            result += text[i++];
        }
    }
    return result;
}

std::string XmlTagToolCallCodec::extractAttribute(const std::string& tag, const std::string& attrName) {
    std::string searchFor = attrName + "=\"";
    size_t pos = tag.find(searchFor);
    if (pos == std::string::npos) {
        return "";
    }
    
    pos += searchFor.length();
    size_t endPos = tag.find('"', pos);
    if (endPos == std::string::npos) {
        return "";
    }
    
    return tag.substr(pos, endPos - pos);
}

std::string XmlTagToolCallCodec::encodeToolCall(const ToolCall& toolCall) {
    std::ostringstream oss;
    
    oss << "<" << TAG_INVOKE << " name=\"" << escapeXml(toolCall.name) << "\">\n";
    
    // 解析参数 JSON
    Json::Value args;
    Json::CharReaderBuilder builder;
    std::istringstream argsStream(toolCall.arguments);
    std::string errors;
    
    if (Json::parseFromStream(builder, argsStream, &args, &errors) && args.isObject()) {
        for (const auto& key : args.getMemberNames()) {
            oss << "<" << TAG_PARAMETER << " name=\"" << escapeXml(key) << "\">";
            
            const auto& value = args[key];
            if (value.isString()) {
                oss << escapeXml(value.asString());
            } else {
                Json::StreamWriterBuilder writer;
                writer["indentation"] = "";
                oss << escapeXml(Json::writeString(writer, value));
            }
            
            oss << "</" << TAG_PARAMETER << ">\n";
        }
    }
    
    oss << "</" << TAG_INVOKE << ">\n";
    
    return oss.str();
}

std::string XmlTagToolCallCodec::encodeToolResult(const ToolResult& result) {
    std::ostringstream oss;
    
    oss << "<" << TAG_TOOL_RESULT << ">\n";
    oss << escapeXml(result.content);
    oss << "\n</" << TAG_TOOL_RESULT << ">";
    
    return oss.str();
}

std::string XmlTagToolCallCodec::encodeToolDefinitions(const Json::Value& tools) {
    if (!tools.isArray() || tools.size() == 0) {
        return "";
    }
    
    std::ostringstream oss;
    
    // Generic tool definition prompt (client-specific "strict" overrides should
    // be injected by higher-level logic that knows the client type).
    oss << "In this environment you have access to a set of tools you can use to answer the user's question.\n\n";
    oss << "To invoke a tool, output ONLY the following XML format:\n\n";
    oss << SENTINEL_START << "\n";
    oss << "<" << TAG_FUNCTION_CALLS_PLAIN << ">\n";
    oss << "<" << TAG_FUNCTION_CALL << ">\n";
    oss << "<" << TAG_TOOL << ">$FUNCTION_NAME</" << TAG_TOOL << ">\n";
    oss << "<" << TAG_ARGS_JSON << "><![CDATA[{\"$PARAMETER_NAME\":\"$PARAMETER_VALUE\"}]]></" << TAG_ARGS_JSON << ">\n";
    oss << "</" << TAG_FUNCTION_CALL << ">\n";
    oss << "</" << TAG_FUNCTION_CALLS_PLAIN << ">\n\n";
    oss << "Here are the functions available in JSONSchema format:\n";
    oss << "<functions>\n";
    
    for (const auto& tool : tools) {
        if (!tool.isObject()) continue;
        
        std::string type = tool.get("type", "").asString();
        if (type != "function") continue;
        
        const auto& func = tool["function"];
        if (!func.isObject()) continue;
        
        std::string name = func.get("name", "").asString();
        std::string description = func.get("description", "").asString();
        
        oss << "<function>";
        
        // 输出描述
        if (!description.empty()) {
            oss << "{\"description\": \"" << escapeXml(description) << "\", ";
        } else {
            oss << "{";
        }
        
        oss << "\"name\": \"" << escapeXml(name) << "\", ";
        
        // 输出参数 schema
        if (func.isMember("parameters")) {
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            oss << "\"parameters\": " << Json::writeString(writer, func["parameters"]);
        } else {
            oss << "\"parameters\": {}";
        }
        
        oss << "}";
        oss << "</function>\n";
    }
    
    oss << "</functions>\n";
    
    return oss.str();
}

bool XmlTagToolCallCodec::decodeIncremental(const std::string& chunk, std::vector<ToolCallEvent>& events) {
    buffer_ += chunk;
    processBuffer(events);
    return true;
}

void XmlTagToolCallCodec::processBuffer(std::vector<ToolCallEvent>& events) {
    auto trimWs = [](std::string s) -> std::string {
        const auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        const auto end = s.find_last_not_of(" \t\n\r");
        return s.substr(start, end - start + 1);
    };

    while (!buffer_.empty()) {
        switch (state_) {
            case XmlParserState::Text: {
                // 查找哨兵或 function_calls 开始标签
                size_t sentinelPos = buffer_.find(SENTINEL_START);
                size_t tagPosAntml = buffer_.find("<" + TAG_FUNCTION_CALLS);
                size_t tagPosPlain = buffer_.find("<" + TAG_FUNCTION_CALLS_PLAIN);

                auto minPos = [](size_t a, size_t b) -> size_t {
                    if (a == std::string::npos) return b;
                    if (b == std::string::npos) return a;
                    return std::min(a, b);
                };
                size_t tagPos = minPos(tagPosAntml, tagPosPlain);
                
                size_t startPos = std::string::npos;
                bool foundSentinel = false;
                
                if (sentinelPos != std::string::npos && (tagPos == std::string::npos || sentinelPos < tagPos)) {
                    startPos = sentinelPos;
                    foundSentinel = true;
                } else if (tagPos != std::string::npos) {
                    startPos = tagPos;
                }
                
                if (startPos == std::string::npos) {
                    // 检查是否可能有不完整的标签
                    size_t potentialTag = buffer_.rfind('<');
                    if (potentialTag != std::string::npos && potentialTag > buffer_.size() - 50) {
                        // 保留可能的不完整标签
                        if (potentialTag > 0) {
                            emitTextEvent(buffer_.substr(0, potentialTag), events);
                            buffer_ = buffer_.substr(potentialTag);
                        }
                    } else {
                        emitTextEvent(buffer_, events);
                        buffer_.clear();
                    }
                    return;
                }
                
                // 输出标签前的文本
                if (startPos > 0) {
                    emitTextEvent(buffer_.substr(0, startPos), events);
                }
                
                if (foundSentinel) {
                    // 跳过哨兵
                    buffer_ = buffer_.substr(startPos + SENTINEL_START.length());
                    // 继续查找 function_calls
                } else {
                    // 找到 function_calls 开始
                    size_t endTag = buffer_.find('>', startPos);
                    if (endTag == std::string::npos) {
                        return; // 等待更多数据
                    }
                    buffer_ = buffer_.substr(endTag + 1);
                    state_ = XmlParserState::InFunctionCalls;
                }
                break;
            }
            
            case XmlParserState::InFunctionCalls: {
                // 跳过空白
                size_t nonWhitespace = buffer_.find_first_not_of(" \t\n\r");
                if (nonWhitespace == std::string::npos) {
                    buffer_.clear();
                    return;
                }
                if (nonWhitespace > 0) {
                    buffer_ = buffer_.substr(nonWhitespace);
                }
                
                // 查找 invoke 或结束标签
                if (buffer_.substr(0, 2) == "</") {
                    size_t endTag = buffer_.find('>');
                    if (endTag == std::string::npos) return;
                    buffer_ = buffer_.substr(endTag + 1);
                    state_ = XmlParserState::Text;
                    break;
                }
                
                const std::string invokePrefixAntml = "<" + TAG_INVOKE;
                const std::string invokePrefixPlain = "<" + TAG_INVOKE_PLAIN;
                const std::string functionCallPrefix = "<" + TAG_FUNCTION_CALL;

                auto startsWith = [](const std::string& s, const std::string& prefix) -> bool {
                    return s.rfind(prefix, 0) == 0;
                };

                if (startsWith(buffer_, functionCallPrefix)) {
                    size_t callEnd = buffer_.find('>');
                    if (callEnd == std::string::npos) return;

                    currentContext_ = ToolCallParseContext();
                    currentContext_.toolCallId = generateToolCallId();
                    currentParamEndTag_.clear();

                    buffer_ = buffer_.substr(callEnd + 1);
                    state_ = XmlParserState::InFunctionCall;
                    break;
                }

                if (!startsWith(buffer_, invokePrefixAntml) && !startsWith(buffer_, invokePrefixPlain)) {
                    // 可能数据不完整或格式不匹配
                    return;
                }

                size_t invokeEnd = buffer_.find('>');
                if (invokeEnd == std::string::npos) return;

                std::string invokeTag = buffer_.substr(0, invokeEnd + 1);

                // 提取工具名称
                currentContext_ = ToolCallParseContext();
                currentContext_.toolName = extractAttribute(invokeTag, "name");
                currentContext_.toolCallId = generateToolCallId();
                currentParamEndTag_.clear();

                emitToolCallBegin(events);

                buffer_ = buffer_.substr(invokeEnd + 1);
                state_ = XmlParserState::InInvoke;
                break;
            }
            
            case XmlParserState::InInvoke: {
                // 跳过空白
                size_t nonWhitespace = buffer_.find_first_not_of(" \t\n\r");
                if (nonWhitespace == std::string::npos) {
                    buffer_.clear();
                    return;
                }
                if (nonWhitespace > 0) {
                    buffer_ = buffer_.substr(nonWhitespace);
                }
                
                // 查找 parameter 或 invoke 结束
                if (buffer_.substr(0, 2) == "</") {
                    size_t endTag = buffer_.find('>');
                    if (endTag == std::string::npos) return;
                    
                    // invoke 结束，发送完整参数
                    emitToolCallEnd(events);
                    
                    buffer_ = buffer_.substr(endTag + 1);
                    state_ = XmlParserState::InFunctionCalls;
                    break;
                }
                
                const std::string paramPrefixAntml = "<" + TAG_PARAMETER;
                const std::string paramPrefixPlain = "<" + TAG_PARAMETER_PLAIN;

                auto startsWith = [](const std::string& s, const std::string& prefix) -> bool {
                    return s.rfind(prefix, 0) == 0;
                };

                std::string paramTagName;
                if (startsWith(buffer_, paramPrefixAntml)) {
                    paramTagName = TAG_PARAMETER;
                } else if (startsWith(buffer_, paramPrefixPlain)) {
                    paramTagName = TAG_PARAMETER_PLAIN;
                } else {
                    return;
                }

                size_t paramEnd = buffer_.find('>');
                if (paramEnd == std::string::npos) return;

                std::string paramTag = buffer_.substr(0, paramEnd + 1);
                currentContext_.currentParamName = extractAttribute(paramTag, "name");
                currentContext_.currentParamValue.clear();
                currentParamEndTag_ = "</" + paramTagName + ">";
                
                buffer_ = buffer_.substr(paramEnd + 1);
                state_ = XmlParserState::InParameter;
                break;
            }
            
            case XmlParserState::InParameter: {
                // 查找参数结束标签
                const std::string endTag = currentParamEndTag_.empty()
                    ? ("</" + TAG_PARAMETER + ">")
                    : currentParamEndTag_;
                size_t endPos = buffer_.find(endTag);
                
                if (endPos == std::string::npos) {
                    // 累积参数值
                    currentContext_.currentParamValue += buffer_;
                    buffer_.clear();
                    return;
                }
                
                currentContext_.currentParamValue += buffer_.substr(0, endPos);
                currentContext_.parameters[currentContext_.currentParamName] = 
                    unescapeXml(currentContext_.currentParamValue);
                
                // 发送参数增量事件
                ToolCallEvent argEvent;
                argEvent.type = EventType::ToolCallArgsDelta;
                argEvent.toolCallId = currentContext_.toolCallId;
                argEvent.toolName = currentContext_.toolName;
                
                // 构建参数 JSON 片段
                Json::Value paramJson;
                paramJson[currentContext_.currentParamName] = currentContext_.parameters[currentContext_.currentParamName];
                Json::StreamWriterBuilder writer;
                writer["indentation"] = "";
                argEvent.argumentsDelta = Json::writeString(writer, paramJson);
                events.push_back(std::move(argEvent));
                
                buffer_ = buffer_.substr(endPos + endTag.length());
                state_ = XmlParserState::InInvoke;
                break;
            }

            case XmlParserState::InFunctionCall: {
                // 跳过空白
                size_t nonWhitespace = buffer_.find_first_not_of(" \t\n\r");
                if (nonWhitespace == std::string::npos) {
                    buffer_.clear();
                    return;
                }
                if (nonWhitespace > 0) {
                    buffer_ = buffer_.substr(nonWhitespace);
                }

                // function_call 结束
                if (buffer_.substr(0, 2) == "</") {
                    size_t endTag = buffer_.find('>');
                    if (endTag == std::string::npos) return;

                    emitToolCallEnd(events);
                    buffer_ = buffer_.substr(endTag + 1);
                    state_ = XmlParserState::InFunctionCalls;
                    break;
                }

                const std::string toolPrefix = "<" + TAG_TOOL;
                const std::string argsPrefix = "<" + TAG_ARGS_JSON;

                auto startsWith = [](const std::string& s, const std::string& prefix) -> bool {
                    return s.rfind(prefix, 0) == 0;
                };

                if (startsWith(buffer_, toolPrefix)) {
                    size_t tagEnd = buffer_.find('>');
                    if (tagEnd == std::string::npos) return;
                    currentContext_.currentParamValue.clear();
                    buffer_ = buffer_.substr(tagEnd + 1);
                    state_ = XmlParserState::InToolTag;
                    break;
                }

                if (startsWith(buffer_, argsPrefix)) {
                    size_t tagEnd = buffer_.find('>');
                    if (tagEnd == std::string::npos) return;
                    currentContext_.currentParamValue.clear();
                    buffer_ = buffer_.substr(tagEnd + 1);
                    state_ = XmlParserState::InArgsJson;
                    break;
                }

                // 未识别到期望的标签，等待更多数据
                return;
            }

            case XmlParserState::InToolTag: {
                const std::string endTag = "</" + TAG_TOOL + ">";
                size_t endPos = buffer_.find(endTag);

                if (endPos == std::string::npos) {
                    currentContext_.currentParamValue += buffer_;
                    buffer_.clear();
                    return;
                }

                currentContext_.currentParamValue += buffer_.substr(0, endPos);
                currentContext_.toolName = trimWs(unescapeXml(currentContext_.currentParamValue));
                buffer_ = buffer_.substr(endPos + endTag.length());
                state_ = XmlParserState::InFunctionCall;
                break;
            }

            case XmlParserState::InArgsJson: {
                const std::string endTag = "</" + TAG_ARGS_JSON + ">";
                size_t endPos = buffer_.find(endTag);

                if (endPos == std::string::npos) {
                    currentContext_.currentParamValue += buffer_;
                    buffer_.clear();
                    return;
                }

                currentContext_.currentParamValue += buffer_.substr(0, endPos);
                std::string payload = trimWs(unescapeXml(currentContext_.currentParamValue));

                // Strip optional CDATA wrapper
                const std::string cdataStart = "<![CDATA[";
                const std::string cdataEnd = "]]>";
                if (payload.size() >= cdataStart.size() + cdataEnd.size() &&
                    payload.rfind(cdataStart, 0) == 0 &&
                    payload.compare(payload.size() - cdataEnd.size(), cdataEnd.size(), cdataEnd) == 0) {
                    payload = payload.substr(cdataStart.size(), payload.size() - cdataStart.size() - cdataEnd.size());
                    payload = trimWs(payload);
                }

                currentContext_.rawArgumentsJson = payload;
                buffer_ = buffer_.substr(endPos + endTag.length());
                state_ = XmlParserState::InFunctionCall;
                break;
            }
            
            default:
                return;
        }
    }
}

void XmlTagToolCallCodec::emitTextEvent(const std::string& text, std::vector<ToolCallEvent>& events) {
    if (text.empty()) return;
    
    ToolCallEvent evt;
    evt.type = EventType::Text;
    evt.text = text;
    events.push_back(std::move(evt));
}

void XmlTagToolCallCodec::emitToolCallBegin(std::vector<ToolCallEvent>& events) {
    ToolCallEvent evt;
    evt.type = EventType::ToolCallBegin;
    evt.toolCallId = currentContext_.toolCallId;
    evt.toolName = currentContext_.toolName;
    events.push_back(std::move(evt));
}

void XmlTagToolCallCodec::emitToolCallEnd(std::vector<ToolCallEvent>& events) {
    ToolCallEvent evt;
    evt.type = EventType::ToolCallEnd;
    evt.toolCallId = currentContext_.toolCallId;
    evt.toolName = currentContext_.toolName;

    // Toolify-style: <args_json> already contains a full JSON object
    if (!currentContext_.rawArgumentsJson.empty()) {
        std::string jsonStr = currentContext_.rawArgumentsJson;

        // Validate it's a JSON object; otherwise wrap it
        Json::Value parsed;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream iss(jsonStr);
        bool ok = Json::parseFromStream(builder, iss, &parsed, &errors);

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";

        if (ok && parsed.isObject()) {
            evt.argumentsDelta = Json::writeString(writer, parsed);
        } else {
            Json::Value wrapper(Json::objectValue);
            wrapper["raw_arguments"] = jsonStr;
            evt.argumentsDelta = Json::writeString(writer, wrapper);
        }
    } else {
        // Parameter-tag style: build full args JSON from collected parameters
        Json::Value args(Json::objectValue);
        for (const auto& [key, value] : currentContext_.parameters) {
            args[key] = value;
        }
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        evt.argumentsDelta = Json::writeString(writer, args);
    }
    
    events.push_back(std::move(evt));
}

void XmlTagToolCallCodec::flush(std::vector<ToolCallEvent>& events) {
    // 刷新任何剩余的文本
    if (!buffer_.empty()) {
        emitTextEvent(buffer_, events);
        buffer_.clear();
    }
    
    // 如果在解析中途，发送错误或不完整事件
    if (state_ != XmlParserState::Text) {
        ToolCallEvent evt;
        evt.type = EventType::Error;
        evt.errorMessage = "Incomplete tool call at end of stream";
        events.push_back(std::move(evt));
        state_ = XmlParserState::Text;
    }
}

void XmlTagToolCallCodec::reset() {
    state_ = XmlParserState::Text;
    buffer_.clear();
    pendingText_.clear();
    currentContext_ = ToolCallParseContext();
    currentParamEndTag_.clear();
}

std::shared_ptr<XmlTagToolCallCodec> createXmlTagToolCallCodec() {
    return std::make_shared<XmlTagToolCallCodec>();
}

} // namespace toolcall
