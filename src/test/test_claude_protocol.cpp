#include <drogon/drogon_test.h>

#include <application/generation/protocol/claude/ClaudeProtocolModule.h>
#include <application/generation/protocol/claude/ClaudeJsonSink.h>
#include <application/generation/protocol/claude/ClaudeSseSink.h>
#include <application/generation/protocol/common/ProtocolRegistry.h>
#include <transport/controllers/AiApiController.h>

#include <algorithm>
#include <string>

using namespace generation::protocol;

namespace {

struct CapturedResponse {
    Json::Value body;
    int status = 0;
    bool called = false;
};

size_t countOccurrences(const std::string& value, const std::string& needle)
{
    size_t count = 0;
    size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

Json::Value textBlock(const std::string& text)
{
    Json::Value block(Json::objectValue);
    block["type"] = "text";
    block["text"] = text;
    return block;
}

}  // namespace

DROGON_TEST(ClaudeProtocol_DefaultRoutesAndCapabilities)
{
    const auto registry = makeDefaultProtocolRegistry();
    REQUIRE(registry);

    const auto* module = registry->findRoute("post", "/chaynsapi/v1/messages");
    REQUIRE(module);
    CHECK(module->id() == "anthropic-messages");
    CHECK(registry->findRouteOperation("POST", "/v1/messages") == "messages.create");

    const auto capabilities = module->capabilityMapper().capabilities("messages.create");
    CHECK(capabilities.text);
    CHECK(capabilities.images);
    CHECK(capabilities.streaming);
    CHECK(capabilities.tools);
    CHECK(capabilities.parallelTools);
    CHECK(capabilities.reasoning);
    CHECK(!capabilities.continuity);
}

DROGON_TEST(ClaudeProtocolModule_OwnsSinkFactory)
{
    auto module = generation::protocol::claude::makeClaudeProtocolModule();
    REQUIRE(module);

    ResponseContext jsonContext;
    jsonContext.operation = "messages.create";
    jsonContext.model = "claude-sonnet-4-6";
    jsonContext.jsonResponse = [](const Json::Value&, int) {};
    auto jsonSink = module->responseSinkFactory("messages.create").create(jsonContext);
    REQUIRE(jsonSink);
    CHECK(jsonSink->getSinkType() == "ClaudeJsonSink");

    ResponseContext streamContext;
    streamContext.operation = "messages.create";
    streamContext.model = "claude-sonnet-4-6";
    streamContext.stream = true;
    streamContext.streamWriter = [](const std::string&) { return true; };
    streamContext.close = [] {};
    auto streamSink = module->responseSinkFactory("messages.create").create(streamContext);
    REQUIRE(streamSink);
    CHECK(streamSink->getSinkType() == "ClaudeSseSink");
}

DROGON_TEST(ClaudeRequestAdapter_MapsCanonicalMessagesAndTools)
{
    auto module = generation::protocol::claude::makeClaudeProtocolModule();
    Json::Value body(Json::objectValue);
    body["model"] = "claude-sonnet-4-6";
    body["max_tokens"] = 512;
    body["stream"] = true;
    body["system"] = "You are precise.";
    body["tool_choice"]["type"] = "any";
    body["tool_choice"]["disable_parallel_tool_use"] = true;
    body["tools"] = Json::Value(Json::arrayValue);
    body["tools"][0]["name"] = "read_file";
    body["tools"][0]["description"] = "Read a file";
    body["tools"][0]["input_schema"]["type"] = "object";

    body["messages"] = Json::Value(Json::arrayValue);
    Json::Value user;
    user["role"] = "user";
    user["content"] = Json::Value(Json::arrayValue);
    user["content"].append(textBlock("old question"));
    body["messages"].append(user);

    Json::Value assistant;
    assistant["role"] = "assistant";
    assistant["content"] = Json::Value(Json::arrayValue);
    assistant["content"].append(textBlock("I will inspect it."));
    Json::Value toolUse;
    toolUse["type"] = "tool_use";
    toolUse["id"] = "toolu_1";
    toolUse["name"] = "read_file";
    toolUse["input"]["path"] = "README.md";
    assistant["content"].append(toolUse);
    body["messages"].append(assistant);

    Json::Value current;
    current["role"] = "user";
    current["content"] = Json::Value(Json::arrayValue);
    current["content"].append(textBlock("new question"));
    Json::Value result;
    result["type"] = "tool_result";
    result["tool_use_id"] = "toolu_1";
    result["content"] = "contents";
    current["content"].append(result);
    body["messages"].append(current);

    RawProtocolRequest raw;
    raw.method = "POST";
    raw.path = "/chaynsapi/v1/messages";
    raw.body = body;
    raw.headers.userAgent = "claude-code/1.0";

    const auto resultValue = module->requestAdapter("messages.create").adapt(raw);
    REQUIRE(resultValue.succeeded());
    const auto& request = *resultValue.request;
    CHECK(request.model == "claude-sonnet-4-6");
    CHECK(request.maxOutputTokens.has_value());
    CHECK(*request.maxOutputTokens == 512);
    CHECK(request.systemPrompt == "You are precise.");
    CHECK(request.messages.size() == 2);
    CHECK(request.messages[1].hasToolUses());
    CHECK(request.currentInput.find("new question") != std::string::npos);
    CHECK(request.currentInput.find("contents") != std::string::npos);
    CHECK(countOccurrences(request.currentInput, "contents") == 1);
    REQUIRE(request.currentInputParts.size() >= 2);
    const auto toolResultPart = std::find_if(
        request.currentInputParts.begin(), request.currentInputParts.end(),
        [](const CurrentInputPart& part) { return part.isToolResult; });
    REQUIRE(toolResultPart != request.currentInputParts.end());
    CHECK(toolResultPart->toolResultCallId == "toolu_1");
    CHECK(toolResultPart->text.find("contents") != std::string::npos);
    CHECK(request.toolDefinitions.size() == 1);
    CHECK(request.toolDefinitions[0].name == "read_file");
    CHECK(request.toolChoiceSpec.mode == ToolChoiceMode::Any);
    CHECK(!request.parallelToolCalls);
    CHECK(request.clientInfo["client_type"].asString() == "ClaudeCode");
    CHECK(request.protocolExtensions["claude"]["raw_tools"].size() == 1);
}

DROGON_TEST(ClaudeRequestAdapter_MapsSystemMessagesToSystemPrompt)
{
    auto module = generation::protocol::claude::makeClaudeProtocolModule();
    Json::Value body(Json::objectValue);
    body["model"] = "claude-sonnet-4-6";
    body["max_tokens"] = 256;
    body["system"] = "base instruction";
    body["messages"] = Json::Value(Json::arrayValue);

    Json::Value user;
    user["role"] = "user";
    user["content"] = "initial question";
    body["messages"].append(user);

    Json::Value system;
    system["role"] = "system";
    system["content"] = Json::Value(Json::arrayValue);
    system["content"].append(textBlock("runtime instruction"));
    body["messages"].append(system);

    Json::Value assistant;
    assistant["role"] = "assistant";
    assistant["content"] = "previous answer";
    body["messages"].append(assistant);

    Json::Value current;
    current["role"] = "user";
    current["content"] = "current question";
    body["messages"].append(current);

    RawProtocolRequest raw;
    raw.body = body;
    const auto result = module->requestAdapter("messages.create").adapt(raw);
    REQUIRE(result.succeeded());
    const auto& request = *result.request;
    CHECK(request.systemPrompt == "base instruction\nruntime instruction");
    CHECK(request.messages.size() == 2);
    CHECK(request.messages[0].role == MessageRole::User);
    CHECK(request.messages[1].role == MessageRole::Assistant);
    CHECK(request.currentInput.find("current question") != std::string::npos);
    CHECK(request.currentInput.find("runtime instruction") == std::string::npos);
    CHECK(request.currentInput.find("initial question") == std::string::npos);
    for (const auto& message : request.messages) {
        CHECK(message.role != MessageRole::System);
    }
}

DROGON_TEST(ClaudeRequestAdapter_RejectsInvalidRequiredFields)
{
    auto module = generation::protocol::claude::makeClaudeProtocolModule();
    RawProtocolRequest raw;
    raw.body["messages"] = Json::Value(Json::arrayValue);
    const auto result = module->requestAdapter("messages.create").adapt(raw);
    CHECK(!result.succeeded());
    REQUIRE(result.error.has_value());
    CHECK(result.error->message == "model is required");
}

DROGON_TEST(ClaudeRequestAdapter_RejectsUnknownMessageRole)
{
    auto module = generation::protocol::claude::makeClaudeProtocolModule();
    Json::Value body(Json::objectValue);
    body["model"] = "claude-sonnet-4-6";
    body["max_tokens"] = 256;
    body["messages"] = Json::Value(Json::arrayValue);
    body["messages"].append(Json::Value(Json::objectValue));
    body["messages"][0]["role"] = "developer";
    body["messages"][0]["content"] = "not a Claude Messages role";

    RawProtocolRequest raw;
    raw.body = body;
    const auto result = module->requestAdapter("messages.create").adapt(raw);
    CHECK(!result.succeeded());
    REQUIRE(result.error.has_value());
    CHECK(result.error->message ==
          "message role must be user, assistant, or system (got developer)");
}

DROGON_TEST(ClaudeRequestAdapter_SplitsParallelToolResultsInHistory)
{
    auto module = generation::protocol::claude::makeClaudeProtocolModule();
    Json::Value body(Json::objectValue);
    body["model"] = "claude-sonnet-4-6";
    body["max_tokens"] = 256;
    body["messages"] = Json::Value(Json::arrayValue);

    Json::Value first;
    first["role"] = "assistant";
    first["content"] = Json::Value(Json::arrayValue);
    Json::Value firstCall;
    firstCall["type"] = "tool_use";
    firstCall["id"] = "toolu_a";
    firstCall["name"] = "first";
    firstCall["input"] = Json::Value(Json::objectValue);
    first["content"].append(firstCall);
    Json::Value secondCall = firstCall;
    secondCall["id"] = "toolu_b";
    secondCall["name"] = "second";
    first["content"].append(secondCall);
    body["messages"].append(first);

    Json::Value results;
    results["role"] = "user";
    results["content"] = Json::Value(Json::arrayValue);
    Json::Value firstResult;
    firstResult["type"] = "tool_result";
    firstResult["tool_use_id"] = "toolu_a";
    firstResult["content"] = "A";
    results["content"].append(firstResult);
    Json::Value secondResult = firstResult;
    secondResult["tool_use_id"] = "toolu_b";
    secondResult["content"] = "B";
    results["content"].append(secondResult);
    body["messages"].append(results);

    Json::Value answer;
    answer["role"] = "assistant";
    answer["content"] = "done";
    body["messages"].append(answer);
    Json::Value current;
    current["role"] = "user";
    current["content"] = "continue";
    body["messages"].append(current);

    RawProtocolRequest raw;
    raw.body = body;
    const auto result = module->requestAdapter("messages.create").adapt(raw);
    REQUIRE(result.succeeded());
    REQUIRE(result.request->messages.size() == 4);
    CHECK(result.request->messages[1].role == MessageRole::Tool);
    CHECK(result.request->messages[1].toolResultCallId() == "toolu_a");
    CHECK(result.request->messages[2].role == MessageRole::Tool);
    CHECK(result.request->messages[2].toolResultCallId() == "toolu_b");
    CHECK(result.request->currentInput.find("continue") != std::string::npos);
}

DROGON_TEST(ClaudeJsonSink_EncodesMessageAndToolUse)
{
    CapturedResponse captured;
    generation::protocol::claude::ClaudeJsonSink sink(
        [&captured](const Json::Value& body, int status) {
            captured.body = body;
            captured.status = status;
            captured.called = true;
        },
        "claude-sonnet-4-6",
        12);

    generation::Started started;
    started.responseId = "conversation-1";
    started.model = "claude-sonnet-4-6";
    sink.onEvent(started);
    generation::OutputTextDelta text;
    text.delta = "hello";
    sink.onEvent(text);
    generation::ToolCallDone call;
    call.id = "toolu_1";
    call.name = "read_file";
    call.arguments = R"({"path":"README.md"})";
    sink.onEvent(call);
    generation::Completed completed;
    completed.finishReason = "tool_calls";
    sink.onEvent(completed);
    sink.onClose();

    REQUIRE(captured.called);
    CHECK(captured.status == 200);
    CHECK(captured.body["type"].asString() == "message");
    CHECK(captured.body["id"].asString() == "msg_conversation-1");
    CHECK(captured.body["role"].asString() == "assistant");
    CHECK(captured.body["content"].size() == 2);
    CHECK(captured.body["content"][0]["type"].asString() == "text");
    CHECK(captured.body["content"][1]["type"].asString() == "tool_use");
    CHECK(captured.body["content"][1]["input"]["path"].asString() == "README.md");
    CHECK(captured.body["stop_reason"].asString() == "tool_use");
    CHECK(captured.body["usage"]["input_tokens"].asInt() == 12);
}

DROGON_TEST(ClaudeSseSink_EncodesAnthropicEventSequenceAndDeduplicatesArguments)
{
    std::string stream;
    int closeCount = 0;
    generation::protocol::claude::ClaudeSseSink sink(
        [&stream](const std::string& chunk) {
            stream += chunk;
            return true;
        },
        [&closeCount] { ++closeCount; },
        "claude-sonnet-4-6");

    generation::Started started;
    started.responseId = "msg_stream";
    sink.onEvent(started);
    generation::ToolCallStarted toolStarted;
    toolStarted.id = "toolu_stream";
    toolStarted.name = "read_file";
    sink.onEvent(toolStarted);
    generation::ToolArgumentsDelta delta;
    delta.id = toolStarted.id;
    delta.delta = R"({"path":)";
    delta.sequence = 0;
    sink.onEvent(delta);
    sink.onEvent(delta);
    delta.delta = R"("README.md"})";
    delta.sequence = 1;
    sink.onEvent(delta);
    generation::ToolCallDone done;
    done.id = toolStarted.id;
    done.name = toolStarted.name;
    done.arguments = R"({"path":"README.md"})";
    sink.onEvent(done);
    generation::Completed completed;
    completed.finishReason = "tool_calls";
    sink.onEvent(completed);
    sink.onClose();

    CHECK(closeCount == 1);
    CHECK(stream.find("event: message_start") != std::string::npos);
    CHECK(stream.find("event: content_block_start") != std::string::npos);
    CHECK(stream.find("input_json_delta") != std::string::npos);
    CHECK(stream.find("event: message_delta") != std::string::npos);
    CHECK(stream.find("event: message_stop") != std::string::npos);
    CHECK(countOccurrences(stream, "partial_json") == 2);
    CHECK(countOccurrences(stream, "event: content_block_stop") == 1);
}

DROGON_TEST(ClaudeJsonSink_FormatsAnthropicError)
{
    CapturedResponse captured;
    generation::protocol::claude::ClaudeJsonSink sink(
        [&captured](const Json::Value& body, int status) {
            captured.body = body;
            captured.status = status;
            captured.called = true;
        },
        "claude-sonnet-4-6");
    generation::Error error;
    error.code = platform::ErrorCode::ProviderError;
    error.message = "upstream failed";
    sink.onEvent(error);
    sink.onClose();

    REQUIRE(captured.called);
    CHECK(captured.status == 502);
    CHECK(captured.body["type"].asString() == "error");
    CHECK(captured.body["error"]["type"].asString() == "api_error");
    CHECK(captured.body["error"]["message"].asString() == "upstream failed");
}

DROGON_TEST(ClaudeController_InvalidJsonUsesAnthropicErrorShape)
{
    AiApiController controller;
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setPath("/v1/messages");
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody("{");

    drogon::HttpResponsePtr captured;
    controller.messagesCreate(
        request,
        [&captured](const drogon::HttpResponsePtr& response) { captured = response; });

    REQUIRE(captured);
    CHECK(captured->getStatusCode() == drogon::k400BadRequest);
    const auto body = captured->getJsonObject();
    REQUIRE(body);
    CHECK((*body)["type"].asString() == "error");
    CHECK((*body)["error"]["type"].asString() == "invalid_request_error");
}
