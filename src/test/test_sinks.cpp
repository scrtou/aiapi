#include <drogon/drogon_test.h>
#include "../controllers/sinks/ChatJsonSink.h"
#include "../controllers/sinks/ResponsesJsonSink.h"
#include "../controllers/sinks/ChatSseSink.h"
#include "../controllers/sinks/ResponsesSseSink.h"

namespace {

struct CapturedResponse {
    Json::Value body;
    int status = 0;
    bool called = false;
};

}

DROGON_TEST(Sinks_ChatJson_TextResponse)
{
    CapturedResponse cap;
    ChatJsonSink sink(
        [&cap](const Json::Value& body, int statusCode) {
            cap.body = body;
            cap.status = statusCode;
            cap.called = true;
        },
        "GPT-4o"
    );

    generation::OutputTextDelta delta;
    delta.delta = "hello";
    sink.onEvent(delta);

    generation::Completed done;
    done.finishReason = "stop";
    sink.onEvent(done);

    sink.onClose();

    CHECK(cap.called);
    CHECK(cap.status == 200);
    CHECK(cap.body["object"].asString() == "chat.completion");
    CHECK(cap.body["choices"][0]["message"]["content"].asString().find("hello") != std::string::npos);
}

DROGON_TEST(Sinks_ChatJson_ToolCalls)
{
    CapturedResponse cap;
    ChatJsonSink sink(
        [&cap](const Json::Value& body, int statusCode) {
            cap.body = body;
            cap.status = statusCode;
            cap.called = true;
        },
        "GPT-4o"
    );

    generation::ToolCallDone tc;
    tc.id = "call_1";
    tc.name = "read_file";
    tc.arguments = R"({"path":"README.md"})";
    sink.onEvent(tc);

    generation::Completed done;
    done.finishReason = "tool_calls";
    sink.onEvent(done);
    sink.onClose();

    CHECK(cap.called);
    CHECK(cap.body["choices"][0]["message"]["tool_calls"].isArray());
    CHECK(cap.body["choices"][0]["message"]["tool_calls"].size() == 1);
    CHECK(cap.body["choices"][0]["message"]["tool_calls"][0]["function"]["name"].asString() == "read_file");
}

DROGON_TEST(Sinks_ChatJson_ErrorResponse)
{
    CapturedResponse cap;
    ChatJsonSink sink(
        [&cap](const Json::Value& body, int statusCode) {
            cap.body = body;
            cap.status = statusCode;
            cap.called = true;
        },
        "GPT-4o"
    );

    generation::Error err;
    err.code = generation::ErrorCode::ProviderError;
    err.message = "upstream failed";
    sink.onEvent(err);

    sink.onClose();

    CHECK(cap.called);
    CHECK(cap.status == 502);
    CHECK(cap.body["error"]["message"].asString() == "upstream failed");
}

DROGON_TEST(Sinks_ResponsesJson_TextResponse)
{
    CapturedResponse cap;
    ResponsesJsonSink sink(
        [&cap](const Json::Value& body, int statusCode) {
            cap.body = body;
            cap.status = statusCode;
            cap.called = true;
        },
        "GPT-4o",
        10
    );

    generation::Started started;
    started.responseId = "resp_1";
    started.model = "GPT-4o";
    sink.onEvent(started);

    generation::OutputTextDelta delta;
    delta.delta = "abc";
    sink.onEvent(delta);

    generation::Completed done;
    done.finishReason = "stop";
    sink.onEvent(done);

    sink.onClose();

    CHECK(cap.called);
    CHECK(cap.status == 200);
    CHECK(cap.body["id"].asString() == "resp_1");
    CHECK(cap.body["output"].isArray());
    CHECK(cap.body["output"][0]["content"][0]["text"].asString() == "abc");
}

DROGON_TEST(Sinks_ResponsesJson_ToolCalls)
{
    CapturedResponse cap;
    ResponsesJsonSink sink(
        [&cap](const Json::Value& body, int statusCode) {
            cap.body = body;
            cap.status = statusCode;
            cap.called = true;
        },
        "GPT-4o"
    );

    generation::Started started;
    started.responseId = "resp_2";
    sink.onEvent(started);

    generation::ToolCallDone tc;
    tc.id = "call_1";
    tc.name = "write_to_file";
    tc.arguments = R"({"path":"a.txt","content":"x"})";
    sink.onEvent(tc);

    generation::Completed done;
    done.finishReason = "tool_calls";
    sink.onEvent(done);
    sink.onClose();

    CHECK(cap.called);
    CHECK(cap.body["output"][0]["tool_calls"].isArray());
    CHECK(cap.body["output"][0]["tool_calls"].size() == 1);
    CHECK(cap.body["output"][0]["tool_calls"][0]["function"]["name"].asString() == "write_to_file");
}

DROGON_TEST(Sinks_ResponsesJson_CodexNativeFunctionCall)
{
    CapturedResponse cap;
    ResponsesJsonSink sink(
        [&cap](const Json::Value& body, int statusCode) {
            cap.body = body;
            cap.status = statusCode;
            cap.called = true;
        },
        "Grok 4.5",
        0,
        true
    );

    generation::Started started;
    started.responseId = "resp_codex_json";
    started.model = "Grok 4.5";
    sink.onEvent(started);

    generation::ToolCallDone call;
    call.id = "call_123";
    call.name = "exec_command";
    call.arguments = R"({"cmd":"ls -la"})";
    call.type = "function";
    sink.onEvent(call);

    generation::Completed completed;
    completed.finishReason = "tool_calls";
    sink.onEvent(completed);
    sink.onClose();

    CHECK(cap.called);
    CHECK(cap.status == 200);
    REQUIRE(cap.body["output"].isArray());
    REQUIRE(cap.body["output"].size() == 1);
    const auto& item = cap.body["output"][0];
    CHECK(item["type"].asString() == "function_call");
    CHECK(item["call_id"].asString() == "call_123");
    CHECK(item["name"].asString() == "exec_command");
    CHECK(item["arguments"].asString() == R"({"cmd":"ls -la"})");
    CHECK(!item.isMember("tool_calls"));
}

DROGON_TEST(Sinks_ResponsesSse_CodexNativeFunctionCallSequence)
{
    std::string stream;
    int closeCount = 0;
    ResponsesSseSink sink(
        [&stream](const std::string& chunk) {
            stream += chunk;
            return true;
        },
        [&closeCount]() {
            ++closeCount;
        },
        "Grok 4.5",
        true
    );

    generation::Started started;
    started.responseId = "resp_codex_sse";
    started.model = "Grok 4.5";
    sink.onEvent(started);

    generation::ToolCallDone call;
    call.id = "call_123";
    call.name = "exec_command";
    call.arguments = R"({"cmd":"ls -la"})";
    call.type = "function";
    sink.onEvent(call);

    generation::Completed completed;
    completed.finishReason = "tool_calls";
    sink.onEvent(completed);
    sink.onClose();

    CHECK(closeCount == 1);
    CHECK(stream.find("event: response.created") != std::string::npos);
    CHECK(stream.find("event: response.output_item.added") != std::string::npos);
    CHECK(stream.find("event: response.function_call_arguments.delta") != std::string::npos);
    CHECK(stream.find("event: response.function_call_arguments.done") != std::string::npos);
    CHECK(stream.find("event: response.output_item.done") != std::string::npos);
    CHECK(stream.find("event: response.completed") != std::string::npos);
    CHECK(stream.find("\"type\":\"function_call\"") != std::string::npos);
    CHECK(stream.find("\"call_id\":\"call_123\"") != std::string::npos);
    CHECK(stream.find("\"name\":\"exec_command\"") != std::string::npos);
    CHECK(stream.find("\"tool_calls\"") == std::string::npos);
    CHECK(stream.find("\"type\":\"message\"") == std::string::npos);
}

DROGON_TEST(Sinks_ChatSse_CloseOnStreamFailure_OnlyOnce)
{
    int closeCount = 0;
    ChatSseSink sink(
        [](const std::string&) {
            return false;
        },
        [&closeCount]() {
            ++closeCount;
        },
        "GPT-4o"
    );

    generation::OutputTextDelta delta;
    delta.delta = "hello";
    sink.onEvent(delta);

    sink.onClose();
    CHECK(closeCount == 1);
}

DROGON_TEST(Sinks_ResponsesSse_CloseOnStreamFailure_OnlyOnce)
{
    int closeCount = 0;
    ResponsesSseSink sink(
        [](const std::string&) {
            return false;
        },
        [&closeCount]() {
            ++closeCount;
        },
        "GPT-4o"
    );

    generation::Started started;
    started.responseId = "resp_1";
    started.model = "GPT-4o";
    sink.onEvent(started);

    sink.onClose();
    CHECK(closeCount == 1);
}
