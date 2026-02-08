#include "sessionManager/tooling/ToolCallBridge.h"
#include <sstream>
#include <random>
#include <iomanip>

namespace toolcall {



ToolCall ToolCall::fromJson(const Json::Value& json) {
    ToolCall tc;
    tc.id = json.get("id", "").asString();
    tc.name = json.get("name", "").asString();
    
    // 可能是字符串或对象
    if (json.isMember("arguments")) {
        if (json["arguments"].isString()) {
            tc.arguments = json["arguments"].asString();
        } else {
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            tc.arguments = Json::writeString(writer, json["arguments"]);
        }
    }
    
    // 兼容 字段格式
    if (json.isMember("function")) {
        const auto& func = json["function"];
        if (tc.name.empty()) {
            tc.name = func.get("name", "").asString();
        }
        if (tc.arguments.empty()) {
            if (func["arguments"].isString()) {
                tc.arguments = func["arguments"].asString();
            } else if (!func["arguments"].isNull()) {
                Json::StreamWriterBuilder writer;
                writer["indentation"] = "";
                tc.arguments = Json::writeString(writer, func["arguments"]);
            }
        }
    }
    
    return tc;
}

Json::Value ToolCall::toJson() const {
    Json::Value json;
    json["id"] = id;
    json["type"] = "function";
    
    Json::Value func;
    func["name"] = name;
    func["arguments"] = arguments;
    json["function"] = func;
    
    return json;
}



ToolResult ToolResult::fromJson(const Json::Value& json) {
    ToolResult tr;
    tr.toolCallId = json.get("tool_call_id", "").asString();
    tr.content = json.get("content", "").asString();
    tr.isError = json.get("is_error", false).asBool();
    return tr;
}

Json::Value ToolResult::toJson() const {
    Json::Value json;
    json["tool_call_id"] = toolCallId;
    json["content"] = content;
    if (isError) {
        json["is_error"] = true;
    }
    return json;
}



ToolCallBridge::ToolCallBridge(const ChannelCapabilities& capabilities)
    : capabilities_(capabilities)
    , codec_(nullptr) {
}

std::vector<Message> ToolCallBridge::transformRequest(
    const std::vector<Message>& messages,
    const Json::Value& tools,
    std::string& systemPrompt) {
    
    // 如果是 模式，直接返回原始消息
    if (!needsTransform()) {
        return messages;
    }
    

    if (!codec_) {
        // 没有 ，无法转换，返回原始消息
        return messages;
    }
    
    // 注入工具定义到
    if (!tools.isNull() && tools.isArray() && tools.size() > 0) {
        std::string toolsPrompt = codec_->encodeToolDefinitions(tools);
        if (!toolsPrompt.empty()) {
            if (!systemPrompt.empty()) {
                systemPrompt += "\n\n";
            }
            systemPrompt += toolsPrompt;
        }
    }
    
    // 转换消息中的 tool_calls 和
    std::vector<Message> transformed;
    transformed.reserve(messages.size());
    
    for (const auto& msg : messages) {
        Message newMsg = msg;
        
        // 处理 消息中的 tool_calls
        if (msg.role == MessageRole::Assistant && !msg.toolCalls.empty()) {
            // 将 工具调用 转换为文本追加到内容
            std::vector<ToolCall> calls;
            for (const auto& tc : msg.toolCalls) {
                calls.push_back(ToolCall::fromJson(tc));
            }
            std::string callsText = transformToolCallsToText(calls);
            
            // 追加到消息内容
            std::string existingContent = newMsg.getTextContent();
            if (!existingContent.empty()) {
                existingContent += "\n";
            }
            existingContent += callsText;
            
            // 清空并重新设置内容
            newMsg.content.clear();
            ContentPart part;
            part.type = ContentPartType::Text;
            part.text = existingContent;
            newMsg.content.push_back(part);
            

            newMsg.toolCalls.clear();
        }
        

        if (msg.role == MessageRole::Tool) {

            newMsg.role = MessageRole::User;
            
            ToolResult result;
            result.toolCallId = msg.toolCallId;
            result.content = msg.getTextContent();
            
            std::string resultText = transformToolResultToText(result);
            
            // 设置新内容
            newMsg.content.clear();
            ContentPart part;
            part.type = ContentPartType::Text;
            part.text = resultText;
            newMsg.content.push_back(part);
            newMsg.toolCallId.clear();
        }
        
        transformed.push_back(std::move(newMsg));
    }
    
    return transformed;
}

std::string ToolCallBridge::transformToolCallsToText(const std::vector<ToolCall>& toolCalls) {
    if (!codec_ || toolCalls.empty()) {
        return "";
    }
    
    std::ostringstream oss;
    for (const auto& tc : toolCalls) {
        oss << codec_->encodeToolCall(tc);
    }
    return oss.str();
}

std::string ToolCallBridge::transformToolResultToText(const ToolResult& result) {
    if (!codec_) {
        return result.content;
    }
    return codec_->encodeToolResult(result);
}

void ToolCallBridge::transformResponseChunk(
    const std::string& chunk,
    std::vector<ToolCallEvent>& events) {
    
    // 如果是 模式，直接输出为文本事件
    if (!needsTransform()) {
        if (!chunk.empty()) {
            ToolCallEvent evt;
            evt.type = EventType::Text;
            evt.text = chunk;
            events.push_back(std::move(evt));
        }
        return;
    }
    

    if (codec_) {
        codec_->decodeIncremental(chunk, events);
    } else {
        // 没有 ，直接输出文本
        if (!chunk.empty()) {
            ToolCallEvent evt;
            evt.type = EventType::Text;
            evt.text = chunk;
            events.push_back(std::move(evt));
        }
    }
}

void ToolCallBridge::flushResponse(std::vector<ToolCallEvent>& events) {
    if (codec_ && needsTransform()) {
        codec_->flush(events);
    }
}

void ToolCallBridge::resetResponseParser() {
    if (codec_) {
        codec_->reset();
    }
}

void ToolCallBridge::setTextCodec(std::shared_ptr<IToolCallTextCodec> codec) {
    codec_ = std::move(codec);
}

// ========== 工厂函数 ==========

std::shared_ptr<ToolCallBridge> createToolCallBridge(bool supportsToolCalls) {
    ChannelCapabilities caps;
    caps.supportsToolCalls = supportsToolCalls;
    caps.supportsStreamingToolCalls = supportsToolCalls;
    
    auto bridge = std::make_shared<ToolCallBridge>(caps);
    
    // 如果不支持 工具调用，后续需要设置
    // 这里先返回，由调用方设置具体的
    
    return bridge;
}

} // 命名空间结束
