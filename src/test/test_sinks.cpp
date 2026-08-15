#include <drogon/drogon_test.h>
#include <transport/controllers/sinks/ChatJsonSink.h>
#include <transport/controllers/sinks/ResponsesJsonSink.h>
#include <transport/controllers/sinks/ChatSseSink.h>
#include <transport/controllers/sinks/ResponsesSseSink.h>

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
    err.code = platform::ErrorCode::ProviderError;
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
    const auto record = sink.responseRecord();
    REQUIRE(record.has_value());
    CHECK(record->responseId == "resp_1");
    CHECK(record->serializedJson.find("resp_1") != std::string::npos);
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
    call.name = "filesystem__read_file";
    call.originalName = "read_file";
    call.namespacePath = "filesystem";
    call.arguments = R"({"path":"README.md"})";
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
    CHECK(item["name"].asString() == "read_file");
    CHECK(item["namespace"].asString() == "filesystem");
    CHECK(item["arguments"].asString() == R"({"path":"README.md"})");
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
    call.name = "filesystem__read_file";
    call.originalName = "read_file";
    call.namespacePath = "filesystem";
    call.arguments = R"({"path":"README.md"})";
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
    CHECK(stream.find("\"name\":\"read_file\"") != std::string::npos);
    CHECK(stream.find("\"namespace\":\"filesystem\"") != std::string::npos);
    CHECK(stream.find("filesystem__read_file") == std::string::npos);
    CHECK(stream.find("\"tool_calls\"") == std::string::npos);
    CHECK(stream.find("\"type\":\"message\"") == std::string::npos);
    const auto record = sink.responseRecord();
    REQUIRE(record.has_value());
    CHECK(record->responseId == "resp_codex_sse");
    CHECK(record->serializedJson.find("function_call") != std::string::npos);
}

DROGON_TEST(Sinks_ChatSse_ConsumesIncrementalToolLifecycle)
{
    std::string stream;
    ChatSseSink sink(
        [&stream](const std::string& chunk) {
            stream += chunk;
            return true;
        },
        [] {},
        "GPT-4o");

    generation::ToolCallStarted started;
    started.id = "call_stream";
    started.name = "read_file";
    sink.onEvent(started);

    generation::ToolArgumentsDelta delta;
    delta.id = "call_stream";
    delta.delta = R"({"path":)";
    delta.sequence = 0;
    sink.onEvent(delta);
    sink.onEvent(delta);
    delta.delta = R"("README.md"})";
    delta.sequence = 1;
    sink.onEvent(delta);

    generation::ToolCallDone done;
    done.id = "call_stream";
    done.name = "read_file";
    done.arguments = R"({"path":"README.md"})";
    sink.onEvent(done);

    generation::Completed completed;
    completed.finishReason = "tool_calls";
    sink.onEvent(completed);
    sink.onClose();

    CHECK(stream.find("call_stream") != std::string::npos);
    CHECK(stream.find("path") != std::string::npos);
    CHECK(stream.find("tool_calls") != std::string::npos);
    CHECK(countOccurrences(stream, "\"arguments\"") == 4);
}

DROGON_TEST(Sinks_ResponsesSse_StreamsIncrementalToolLifecycleOnce)
{
    std::string stream;
    ResponsesSseSink sink(
        [&stream](const std::string& chunk) {
            stream += chunk;
            return true;
        },
        [] {},
        "GPT-4o",
        true);

    generation::Started responseStarted;
    responseStarted.responseId = "resp_incremental_tool";
    responseStarted.model = "GPT-4o";
    sink.onEvent(responseStarted);

    generation::ToolCallStarted started;
    started.id = "call_incremental";
    started.name = "read_file";
    sink.onEvent(started);

    generation::ToolArgumentsDelta delta;
    delta.id = started.id;
    delta.delta = R"({"path":)";
    delta.sequence = 0;
    sink.onEvent(delta);
    sink.onEvent(delta);
    delta.delta = R"("README.md"})";
    delta.sequence = 1;
    sink.onEvent(delta);

    generation::ToolCallDone done;
    done.id = started.id;
    done.name = started.name;
    done.arguments = R"({"path":"README.md"})";
    sink.onEvent(done);
    sink.onEvent(done);

    generation::Completed completed;
    completed.finishReason = "tool_calls";
    sink.onEvent(completed);
    sink.onClose();

    CHECK(countOccurrences(stream, "event: response.output_item.added") == 1);
    CHECK(countOccurrences(stream, "event: response.function_call_arguments.delta") == 2);
    CHECK(countOccurrences(stream, "event: response.function_call_arguments.done") == 1);
    CHECK(countOccurrences(stream, "event: response.output_item.done") == 1);
    const auto record = sink.responseRecord();
    REQUIRE(record.has_value());
    CHECK(countOccurrences(record->serializedJson, "call_incremental") == 2);
}

DROGON_TEST(Sinks_ResponsesSse_ParallelToolsKeepOutputIndexOrder)
{
    std::string stream;
    ResponsesSseSink sink(
        [&stream](const std::string& chunk) {
            stream += chunk;
            return true;
        },
        [] {},
        "GPT-4o",
        true);

    generation::Started responseStarted;
    responseStarted.responseId = "resp_parallel_tools";
    sink.onEvent(responseStarted);

    generation::ToolCallStarted firstStarted;
    firstStarted.id = "call_first";
    firstStarted.name = "first_tool";
    firstStarted.index = 0;
    sink.onEvent(firstStarted);

    generation::ToolCallStarted secondStarted;
    secondStarted.id = "call_second";
    secondStarted.name = "second_tool";
    secondStarted.index = 1;
    sink.onEvent(secondStarted);

    generation::ToolCallDone secondDone;
    secondDone.id = secondStarted.id;
    secondDone.name = secondStarted.name;
    secondDone.index = secondStarted.index;
    secondDone.arguments = R"({"value":2})";
    sink.onEvent(secondDone);

    generation::ToolCallDone firstDone;
    firstDone.id = firstStarted.id;
    firstDone.name = firstStarted.name;
    firstDone.index = firstStarted.index;
    firstDone.arguments = R"({"value":1})";
    sink.onEvent(firstDone);

    generation::Completed completed;
    completed.finishReason = "tool_calls";
    sink.onEvent(completed);
    sink.onClose();

    const auto outputOffset = stream.rfind("\"output\":[");
    REQUIRE(outputOffset != std::string::npos);
    const auto finalOutput = stream.substr(outputOffset);
    const auto firstOffset = finalOutput.find("call_first");
    const auto secondOffset = finalOutput.find("call_second");
    REQUIRE(firstOffset != std::string::npos);
    REQUIRE(secondOffset != std::string::npos);
    CHECK(firstOffset < secondOffset);

    const auto record = sink.responseRecord();
    REQUIRE(record.has_value());
    const auto recordFirst = record->serializedJson.find("call_first");
    const auto recordSecond = record->serializedJson.find("call_second");
    REQUIRE(recordFirst != std::string::npos);
    REQUIRE(recordSecond != std::string::npos);
    CHECK(recordFirst < recordSecond);
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
