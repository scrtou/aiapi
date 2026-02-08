#include <drogon/drogon_test.h>
#include "sessionManager/tooling/ToolCallBridge.h"
#include "sessionManager/tooling/XmlTagToolCallCodec.h"
using namespace toolcall;

namespace {

std::vector<ToolCallEvent> parseXml(const std::string& payload, const std::string& sentinel = "") {
    auto bridge = createToolCallBridge(false);
    auto codec = createXmlTagToolCallCodec();
    if (!sentinel.empty()) {
        codec->setSentinel(sentinel);
    }
    bridge->setTextCodec(codec);

    std::vector<ToolCallEvent> events;
    bridge->transformResponseChunk(payload, events);
    bridge->flushResponse(events);
    return events;
}

size_t countEvent(const std::vector<ToolCallEvent>& events, EventType type) {
    size_t count = 0;
    for (const auto& event : events) {
        if (event.type == type) {
            ++count;
        }
    }
    return count;
}

}

DROGON_TEST(XmlCodec_EncodeToolDefinitions_Basic)
{
    auto codec = createXmlTagToolCallCodec();

    Json::Value tools(Json::arrayValue);
    Json::Value tool;
    tool["type"] = "function";
    tool["function"]["name"] = "read_file";
    tool["function"]["description"] = "Read file content";
    tools.append(tool);

    const std::string encoded = codec->encodeToolDefinitions(tools);
    CHECK(encoded.find("read_file") != std::string::npos);
    CHECK(encoded.find("function_calls") != std::string::npos);
}

DROGON_TEST(XmlCodec_ParseToolCall_WithSentinel)
{
    const std::string sentinel = "<Function_Ab1c_Start/>";
    const std::string xml = sentinel + R"(
<function_calls>
  <function_call>
    <tool>read_file</tool>
    <args_json>{"path":"README.md"}</args_json>
  </function_call>
</function_calls>
)";

    const auto events = parseXml(xml, sentinel);
    CHECK(countEvent(events, EventType::ToolCallEnd) >= 1);

    bool found = false;
    for (const auto& event : events) {
        if (event.type == EventType::ToolCallEnd && event.toolName == "read_file") {
            found = true;
            CHECK(event.argumentsDelta.find("README.md") != std::string::npos);
        }
    }
    CHECK(found);
}

DROGON_TEST(XmlCodec_ParseToolCall_WithoutSentinel)
{
    const std::string xml = R"(
<function_calls>
  <function_call>
    <tool>attempt_completion</tool>
    <args_json>{"result":"done"}</args_json>
  </function_call>
</function_calls>
)";

    const auto events = parseXml(xml);
    CHECK(countEvent(events, EventType::ToolCallEnd) >= 1);

    bool found = false;
    for (const auto& event : events) {
        if (event.type == EventType::ToolCallEnd && event.toolName == "attempt_completion") {
            found = true;
            CHECK(event.argumentsDelta.find("done") != std::string::npos);
        }
    }
    CHECK(found);
}

DROGON_TEST(XmlCodec_ParseMixedTextAndTool)
{
    const std::string xml = R"(
normal intro text
<function_calls>
  <function_call>
    <tool>read_file</tool>
    <args_json>{"path":"src/main.cc"}</args_json>
  </function_call>
</function_calls>
)";

    const auto events = parseXml(xml);
    CHECK(countEvent(events, EventType::Text) >= 1);
    CHECK(countEvent(events, EventType::ToolCallEnd) >= 1);
}

DROGON_TEST(XmlCodec_ParseIncompletePayload_NoCrash)
{
    const std::string broken = R"(
<function_calls>
  <function_call>
    <tool>read_file</tool>
    <args_json>{"path":"src/main.cc"
)";

    const auto events = parseXml(broken);
    CHECK(!events.empty());
    CHECK(countEvent(events, EventType::Error) >= 0);
}
