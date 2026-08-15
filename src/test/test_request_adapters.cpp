#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <sessionManager/core/RequestAdapters.h>
using namespace drogon;

namespace {

class FakeAccountSettings final : public IAccountSettingsQuery
{
  public:
    AccountAutomationSettings settings;
    AccountAutomationSettings getAccountAutomationSettings() const override
    { return settings; }
};

HttpRequestPtr makeJsonRequest(const Json::Value& body,
                               const std::string& ua = "",
                               const std::string& auth = "") {
    auto req = HttpRequest::newHttpJsonRequest(body);
    if (!ua.empty()) {
        req->addHeader("user-agent", ua);
    }
    if (!auth.empty()) {
        req->addHeader("Authorization", auth);
    }
    return req;
}

aiapi::RequestHeaders headersFromRequest(const HttpRequestPtr& req)
{
    aiapi::RequestHeaders headers;
    if (!req) return headers;
    headers.requestId = req->getHeader("x-request-id");
    headers.correlationId = req->getHeader("x-correlation-id");
    headers.userAgent = req->getHeader("user-agent");
    headers.originator = req->getHeader("originator");
    headers.codexWindowId = req->getHeader("x-codex-window-id");
    headers.threadId = req->getHeader("thread-id");
    headers.sessionId = req->getHeader("session-id");
    headers.sessionIdUnderscore = req->getHeader("session_id");
    headers.conversationId = req->getHeader("conversation-id");
    headers.conversationIdUnderscore = req->getHeader("conversation_id");
    headers.authorization = req->getHeader("authorization");
    if (headers.authorization.empty()) headers.authorization = req->getHeader("Authorization");
    return headers;
}

GenerationRequest buildChatRequest(const HttpRequestPtr& req)
{
    const auto body = req ? req->getJsonObject() : nullptr;
    return RequestAdapters::buildGenerationRequestFromChat(
        body ? *body : Json::Value(), headersFromRequest(req));
}

GenerationRequest buildResponsesRequest(const HttpRequestPtr& req)
{
    const auto body = req ? req->getJsonObject() : nullptr;
    return RequestAdapters::buildGenerationRequestFromResponses(
        body ? *body : Json::Value(), headersFromRequest(req));
}

}

DROGON_TEST(RequestAdapters_Chat_BasicFields)
{
    Json::Value body;
    body["model"] = "GPT-4o";
    body["stream"] = true;

    Json::Value msgs(Json::arrayValue);
    {
        Json::Value m;
        m["role"] = "system";
        m["content"] = "you are test";
        msgs.append(m);
    }
    {
        Json::Value m;
        m["role"] = "user";
        m["content"] = "hello";
        msgs.append(m);
    }
    body["messages"] = msgs;

    auto req = makeJsonRequest(body, "Kilo-Code/1.0", "Bearer test-key");
    auto genReq = buildChatRequest(req);

    CHECK(genReq.endpointType == EndpointType::ChatCompletions);
    CHECK(genReq.model == "GPT-4o");
    CHECK(genReq.stream);
    CHECK(genReq.systemPrompt.find("you are test") != std::string::npos);
    CHECK(genReq.currentInput.find("hello") != std::string::npos);
    CHECK(genReq.clientInfo["client_type"].asString() == "Kilo-Code");
    CHECK(genReq.clientInfo["client_authorization"].asString() == "test-key");
    CHECK(genReq.requestId.rfind("req_", 0) == 0);
}

DROGON_TEST(RequestAdapters_Chat_ContentArrayWithImage)
{
    Json::Value body;
    body["model"] = "GPT-4o";

    Json::Value msgs(Json::arrayValue);
    Json::Value user;
    user["role"] = "user";
    Json::Value content(Json::arrayValue);
    {
        Json::Value textPart;
        textPart["type"] = "text";
        textPart["text"] = "look this image";
        content.append(textPart);
    }
    {
        Json::Value imgPart;
        imgPart["type"] = "image_url";
        Json::Value imageUrl;
        imageUrl["url"] = "https://example.com/a.png";
        imgPart["image_url"] = imageUrl;
        content.append(imgPart);
    }
    user["content"] = content;
    msgs.append(user);
    body["messages"] = msgs;

    auto req = makeJsonRequest(body);
    auto genReq = buildChatRequest(req);

    CHECK(genReq.currentInput.find("look this image") != std::string::npos);
    CHECK(genReq.images.size() == 1);
    CHECK(genReq.images[0].uploadedUrl == "https://example.com/a.png");
}

DROGON_TEST(RequestAdapters_Chat_ToolsAndToolChoice)
{
    Json::Value body;
    body["model"] = "GPT-4o";

    Json::Value msgs(Json::arrayValue);
    Json::Value user;
    user["role"] = "user";
    user["content"] = "do tool";
    msgs.append(user);
    body["messages"] = msgs;

    Json::Value tools(Json::arrayValue);
    Json::Value t;
    t["type"] = "function";
    t["function"]["name"] = "read_file";
    tools.append(t);
    body["tools"] = tools;

    Json::Value choice;
    choice["type"] = "function";
    choice["function"]["name"] = "read_file";
    body["tool_choice"] = choice;

    auto req = makeJsonRequest(body);
    auto genReq = buildChatRequest(req);

    CHECK(genReq.tools.isArray());
    CHECK(genReq.tools.size() == 1);
    CHECK(genReq.toolChoice.find("read_file") != std::string::npos);
}

DROGON_TEST(RequestAdapters_Responses_NamespaceToolsPreserveRawTreeAndBridgeLeaves)
{
    Json::Value body;
    body["model"] = "GPT-4o";
    body["input"] = "use the namespaced tool";

    Json::Value namespaceTool;
    namespaceTool["type"] = "namespace";
    namespaceTool["name"] = "filesystem";

    Json::Value nested(Json::arrayValue);
    Json::Value function;
    function["type"] = "function";
    function["name"] = "read_file";
    function["description"] = "Read a file";
    function["parameters"]["type"] = "object";
    function["parameters"]["properties"]["path"]["type"] = "string";
    nested.append(function);
    namespaceTool["tools"] = nested;

    Json::Value tools(Json::arrayValue);
    tools.append(namespaceTool);
    body["tools"] = tools;

    const auto genReq = buildResponsesRequest(
        makeJsonRequest(body));

    REQUIRE(genReq.toolsRaw.isArray());
    REQUIRE(genReq.toolsRaw.size() == 1);
    CHECK(genReq.toolsRaw[0]["type"].asString() == "namespace");
    CHECK(genReq.toolsRaw[0]["name"].asString() == "filesystem");
    REQUIRE(genReq.toolsRaw[0]["tools"].isArray());
    CHECK(genReq.toolsRaw[0]["tools"][0]["name"].asString() == "read_file");

    REQUIRE(genReq.tools.isArray());
    REQUIRE(genReq.tools.size() == 1);
    const auto& bridged = genReq.tools[0]["function"];
    CHECK(bridged["name"].asString() == "filesystem__read_file");
    CHECK(bridged["_aiapi_original_name"].asString() == "read_file");
    CHECK(bridged["_aiapi_namespace"].asString() == "filesystem");
}

DROGON_TEST(RequestAdaptersNamespaceBridgeReadsInjectedSettings)
{
    FakeAccountSettings settings;
    settings.settings.namespaceToolBridgeEnabled = false;
    RequestAdapters::setAccountSettingsQuery(&settings);

    Json::Value body;
    body["model"] = "GPT-4o";
    body["input"] = "namespace disabled";
    Json::Value namespaceTool;
    namespaceTool["type"] = "namespace";
    namespaceTool["name"] = "filesystem";
    Json::Value function;
    function["type"] = "function";
    function["name"] = "read_file";
    namespaceTool["tools"].append(function);
    body["tools"].append(namespaceTool);

    const auto request = buildResponsesRequest(
        makeJsonRequest(body));
    CHECK(request.tools.empty());
    REQUIRE(request.toolsRaw.size() == 1);
    RequestAdapters::setAccountSettingsQuery(nullptr);
}

DROGON_TEST(RequestAdapters_Responses_NestedNamespacesAndDuplicateLeafNamesStayDistinct)
{
    Json::Value body;
    body["model"] = "GPT-4o";
    body["input"] = "use nested tools";

    auto makeFunction = [](const std::string& name) {
        Json::Value function;
        function["type"] = "function";
        function["name"] = name;
        function["parameters"]["type"] = "object";
        function["parameters"]["properties"]["path"]["type"] = "string";
        return function;
    };

    Json::Value workspace;
    workspace["type"] = "namespace";
    workspace["name"] = "workspace";
    Json::Value filesystem;
    filesystem["type"] = "namespace";
    filesystem["name"] = "filesystem";
    filesystem["tools"].append(makeFunction("read_file"));
    workspace["tools"].append(filesystem);

    Json::Value archive;
    archive["type"] = "namespace";
    archive["name"] = "archive";
    archive["tools"].append(makeFunction("read_file"));

    body["tools"].append(workspace);
    body["tools"].append(archive);

    const auto genReq = buildResponsesRequest(
        makeJsonRequest(body));

    REQUIRE(genReq.toolsRaw.size() == 2);
    REQUIRE(genReq.tools.size() == 2);
    CHECK(genReq.tools[0]["function"]["name"].asString() ==
          "workspace__filesystem__read_file");
    CHECK(genReq.tools[0]["function"]["_aiapi_namespace"].asString() ==
          "workspace__filesystem");
    CHECK(genReq.tools[1]["function"]["name"].asString() ==
          "archive__read_file");
    CHECK(genReq.tools[1]["function"]["_aiapi_namespace"].asString() ==
          "archive");
}

DROGON_TEST(RequestAdapters_Responses_StringInputAndPreviousResponse)
{
    Json::Value body;
    body["model"] = "GPT-4o-mini";
    body["instructions"] = "keep short";
    body["input"] = "new prompt";
    body["previous_response_id"] = "resp_123";

    auto req = makeJsonRequest(body, "RooCode/2.0");
    auto genReq = buildResponsesRequest(req);

    CHECK(genReq.endpointType == EndpointType::Responses);
    CHECK(genReq.model == "GPT-4o-mini");
    CHECK(genReq.systemPrompt == "keep short");
    CHECK(genReq.currentInput.find("new prompt") != std::string::npos);
    CHECK(genReq.previousResponseId.has_value());
    CHECK(*genReq.previousResponseId == "resp_123");
    CHECK(genReq.clientInfo["client_type"].asString() == "RooCode");
    CHECK(genReq.requestId.rfind("req_", 0) == 0);
}

DROGON_TEST(RequestAdapters_Responses_InputSystemAndDeveloperInstructions)
{
    Json::Value body;
    body["model"] = "GPT-4o-mini";
    body["instructions"] = "base instruction";

    Json::Value input(Json::arrayValue);
    {
        Json::Value message;
        message["role"] = "system";
        message["content"] = "system instruction";
        input.append(message);
    }
    {
        Json::Value message;
        message["role"] = "developer";
        Json::Value content(Json::arrayValue);
        Json::Value part;
        part["type"] = "input_text";
        part["text"] = "developer instruction";
        content.append(part);
        message["content"] = content;
        input.append(message);
    }
    {
        Json::Value message;
        message["role"] = "user";
        message["content"] = "question";
        input.append(message);
    }
    body["input"] = input;

    const auto genReq = buildResponsesRequest(
        makeJsonRequest(body));

    CHECK(genReq.systemPrompt.find("base instruction") != std::string::npos);
    CHECK(genReq.systemPrompt.find("system instruction") != std::string::npos);
    CHECK(genReq.systemPrompt.find("developer instruction") != std::string::npos);
    CHECK(genReq.currentInput.find("question") != std::string::npos);
}

DROGON_TEST(RequestAdapters_RequestIdHeaders)
{
    Json::Value body;
    body["model"] = "GPT-4o";
    body["messages"] = Json::Value(Json::arrayValue);

    auto requestWithBoth = makeJsonRequest(body);
    requestWithBoth->addHeader("x-request-id", "client-request-123");
    requestWithBoth->addHeader("x-correlation-id", "correlation-ignored");
    const auto chatReq = buildChatRequest(requestWithBoth);
    CHECK(chatReq.requestId == "client-request-123");

    auto correlationOnly = makeJsonRequest(body);
    correlationOnly->addHeader("x-correlation-id", "corr:456");
    const auto responsesReq = buildResponsesRequest(correlationOnly);
    CHECK(responsesReq.requestId == "corr:456");
}

DROGON_TEST(RequestAdapters_CodexCliStableSessionFromHeader)
{
    Json::Value body;
    body["model"] = "GPT-4o";
    body["input"] = "hello";

    auto req = makeJsonRequest(body, "codex_cli_rs/0.133.0");
    req->addHeader("session-id", "codex-session-123");

    const auto genReq = buildResponsesRequest(req);
    CHECK(genReq.clientInfo["client_type"].asString() == "Codex");
    CHECK(genReq.clientInfo["client_session_id"].asString() == "codex-session-123");
    CHECK(genReq.clientInfo["client_session_source"].asString() == "header.session-id");
}

DROGON_TEST(RequestAdapters_CodexCliStableSessionFromBodyFallback)
{
    Json::Value body;
    body["model"] = "GPT-4o";
    body["input"] = "hello";
    body["conversation_id"] = "conversation-body-456";

    const auto genReq = buildResponsesRequest(
        makeJsonRequest(body, "codex_cli_rs/0.133.0"));

    CHECK(genReq.clientInfo["client_type"].asString() == "Codex");
    CHECK(genReq.clientInfo["client_session_id"].asString() == "conversation-body-456");
    CHECK(genReq.clientInfo["client_session_source"].asString() == "body.conversation_id");
}

DROGON_TEST(RequestAdapters_CodexWindowIdAloneDoesNotEnableCodexProtocol)
{
    Json::Value body;
    body["model"] = "GPT-4o";
    body["input"] = "hello";

    auto req = makeJsonRequest(body, "compatible-client/1.0");
    req->addHeader("x-codex-window-id", "window-only-789");
    const auto genReq = buildResponsesRequest(req);

    CHECK(genReq.clientInfo["client_type"].asString() == "compatible-client/1.0");
    CHECK(!genReq.clientInfo.isMember("client_session_id"));
}

DROGON_TEST(RequestAdapters_RealCodexMayIncludeWindowId)
{
    Json::Value body;
    body["model"] = "GPT-4o";
    body["input"] = "hello";

    auto req = makeJsonRequest(body, "codex_cli_rs/0.133.0");
    req->addHeader("x-codex-window-id", "window-789");
    const auto genReq = buildResponsesRequest(req);

    CHECK(genReq.clientInfo["client_type"].asString() == "Codex");
    CHECK(!genReq.clientInfo.isMember("client_session_id"));
}

DROGON_TEST(RequestAdapters_Responses_InputItems)
{
    Json::Value body;
    body["model"] = "GPT-4o";

    Json::Value inputItems(Json::arrayValue);
    Json::Value textItem;
    textItem["type"] = "input_text";
    textItem["text"] = "from input_items";
    inputItems.append(textItem);

    Json::Value imgItem;
    imgItem["type"] = "input_image";
    imgItem["image_url"] = "https://example.com/b.png";
    inputItems.append(imgItem);

    body["input_items"] = inputItems;

    auto req = makeJsonRequest(body);
    auto genReq = buildResponsesRequest(req);

    CHECK(genReq.currentInput.find("from input_items") != std::string::npos);
    CHECK(genReq.images.size() == 1);
    CHECK(genReq.images[0].uploadedUrl == "https://example.com/b.png");
    CHECK(!genReq.continuityTexts.empty());
}

DROGON_TEST(RequestAdapters_Responses_FullTranscriptDoesNotReplayHistoricalToolOutput)
{
    Json::Value body;
    body["model"] = "Grok 4.5";

    Json::Value input(Json::arrayValue);
    {
        Json::Value item;
        item["type"] = "message";
        item["role"] = "user";
        item["content"] = "old question";
        input.append(item);
    }
    {
        Json::Value item;
        item["type"] = "function_call";
        item["call_id"] = "call_old";
        item["name"] = "read_file";
        item["arguments"] = R"({"path":"old.txt"})";
        input.append(item);
    }
    {
        Json::Value item;
        item["type"] = "function_call_output";
        item["call_id"] = "call_old";
        item["output"] = "HISTORICAL_TOOL_OUTPUT_MUST_NOT_BE_CURRENT";
        input.append(item);
    }
    {
        Json::Value item;
        item["type"] = "message";
        item["role"] = "assistant";
        item["content"] = "old task finished";
        input.append(item);
    }
    {
        Json::Value item;
        item["type"] = "message";
        item["role"] = "user";
        item["content"] = "new question";
        input.append(item);
    }
    body["input"] = input;

    const auto genReq = buildResponsesRequest(
        makeJsonRequest(body, "codex_cli_rs/0.133.0"));

    CHECK(genReq.currentInput.find("new question") != std::string::npos);
    CHECK(genReq.currentInput.find("HISTORICAL_TOOL_OUTPUT_MUST_NOT_BE_CURRENT") ==
          std::string::npos);

    bool foundHistoricalToolResult = false;
    for (const auto& message : genReq.messages) {
        if (message.role == MessageRole::Tool &&
            message.toolCallId == "call_old" &&
            message.getTextContent().find("HISTORICAL_TOOL_OUTPUT_MUST_NOT_BE_CURRENT") !=
                std::string::npos) {
            foundHistoricalToolResult = true;
        }
    }
    CHECK(foundHistoricalToolResult);
}

DROGON_TEST(RequestAdapters_Responses_XmlBridgeKeepsLatestParallelToolResults)
{
    Json::Value body;
    body["model"] = "Grok 4.5";

    Json::Value input(Json::arrayValue);
    {
        Json::Value item;
        item["type"] = "message";
        item["role"] = "user";
        item["content"] = "read both files";
        input.append(item);
    }
    for (const auto& callId : {"call_a", "call_b"}) {
        Json::Value item;
        item["type"] = "function_call";
        item["call_id"] = callId;
        item["name"] = "read_file";
        item["arguments"] = "{}";
        input.append(item);
    }
    {
        Json::Value item;
        item["type"] = "function_call_output";
        item["call_id"] = "call_a";
        item["output"] = "LATEST_RESULT_A";
        input.append(item);
    }
    {
        Json::Value item;
        item["type"] = "function_call_output";
        item["call_id"] = "call_b";
        item["output"] = "LATEST_RESULT_B";
        input.append(item);
    }
    body["input"] = input;

    const auto genReq = buildResponsesRequest(
        makeJsonRequest(body, "codex_cli_rs/0.133.0"));

    CHECK(genReq.currentInput.find("LATEST_RESULT_A") != std::string::npos);
    CHECK(genReq.currentInput.find("LATEST_RESULT_B") != std::string::npos);
    CHECK(genReq.currentInput.find("call_id=call_a") != std::string::npos);
    CHECK(genReq.currentInput.find("call_id=call_b") != std::string::npos);
}

DROGON_TEST(RequestAdapters_Responses_XmlBridgeKeepsIncrementalToolOutputWithoutBoundary)
{
    Json::Value body;
    body["model"] = "Grok 4.5";

    Json::Value input(Json::arrayValue);
    Json::Value item;
    item["type"] = "custom_tool_call_output";
    item["call_id"] = "call_incremental";
    item["output"] = "INCREMENTAL_TOOL_RESULT";
    input.append(item);
    body["input"] = input;

    const auto genReq = buildResponsesRequest(
        makeJsonRequest(body, "codex_cli_rs/0.133.0"));

    CHECK(genReq.currentInput.find("INCREMENTAL_TOOL_RESULT") != std::string::npos);
    CHECK(genReq.currentInput.find("call_id=call_incremental") != std::string::npos);
}

DROGON_TEST(RequestAdapters_Responses_XmlBridgeOnlyReplaysNewestToolCycle)
{
    Json::Value body;
    body["model"] = "Grok 4.5";

    Json::Value input(Json::arrayValue);
    auto appendMessage = [&](const std::string& role, const std::string& content) {
        Json::Value item;
        item["type"] = "message";
        item["role"] = role;
        item["content"] = content;
        input.append(item);
    };
    auto appendCall = [&](const std::string& callId) {
        Json::Value item;
        item["type"] = "function_call";
        item["call_id"] = callId;
        item["name"] = "read_file";
        item["arguments"] = "{}";
        input.append(item);
    };
    auto appendOutput = [&](const std::string& callId, const std::string& output) {
        Json::Value item;
        item["type"] = "function_call_output";
        item["call_id"] = callId;
        item["output"] = output;
        input.append(item);
    };

    appendMessage("user", "old request");
    appendCall("call_old_cycle");
    appendOutput("call_old_cycle", "OLD_CYCLE_RESULT");
    appendMessage("assistant", "old cycle complete");
    appendMessage("user", "new request that caused the latest XML tool call");
    appendCall("call_new_cycle");
    appendOutput("call_new_cycle", "NEW_CYCLE_RESULT");
    body["input"] = input;

    const auto genReq = buildResponsesRequest(
        makeJsonRequest(body, "codex_cli_rs/0.133.0"));

    CHECK(genReq.currentInput.find("NEW_CYCLE_RESULT") != std::string::npos);
    CHECK(genReq.currentInput.find("OLD_CYCLE_RESULT") == std::string::npos);
    CHECK(genReq.currentInput.find("new request that caused") == std::string::npos);
}
