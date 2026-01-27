#ifndef XML_TAG_TOOL_CALL_CODEC_H
#define XML_TAG_TOOL_CALL_CODEC_H

#include "ToolCallBridge.h"
#include <string>
#include <vector>
#include <map>

namespace toolcall {

enum class XmlParserState {
    Text,
    MaybeTagStart,
    InFunctionCalls,
    InInvoke,
    InParameter,
    InFunctionCall,
    InToolTag,
    InArgsJson
};

struct ToolCallParseContext {
    std::string toolCallId;
    std::string toolName;
    std::string currentParamName;
    std::string currentParamValue;
    std::map<std::string, std::string> parameters;
    std::string rawArgumentsJson;  // Toolify-style <args_json> payload
    bool isComplete = false;
};

class XmlTagToolCallCodec : public IToolCallTextCodec {
public:
    XmlTagToolCallCodec();
    ~XmlTagToolCallCodec() override = default;
    
    std::string encodeToolCall(const ToolCall& toolCall) override;
    std::string encodeToolResult(const ToolResult& result) override;
    std::string encodeToolDefinitions(const Json::Value& tools) override;
    bool decodeIncremental(const std::string& chunk, std::vector<ToolCallEvent>& events) override;
    void flush(std::vector<ToolCallEvent>& events) override;
    void reset() override;
    
private:
    XmlParserState state_;
    std::string buffer_;
    std::string pendingText_;
    ToolCallParseContext currentContext_;
    std::string currentParamEndTag_;
    int toolCallCounter_;
    
    void processBuffer(std::vector<ToolCallEvent>& events);
    void emitTextEvent(const std::string& text, std::vector<ToolCallEvent>& events);
    void emitToolCallBegin(std::vector<ToolCallEvent>& events);
    void emitToolCallEnd(std::vector<ToolCallEvent>& events);
    std::string generateToolCallId();
    std::string extractAttribute(const std::string& tag, const std::string& attrName);
    std::string escapeXml(const std::string& text);
    std::string unescapeXml(const std::string& text);
};

std::shared_ptr<XmlTagToolCallCodec> createXmlTagToolCallCodec();

} // namespace toolcall

#endif // XML_TAG_TOOL_CALL_CODEC_H
