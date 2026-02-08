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
