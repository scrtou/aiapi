#include <drogon/drogon_test.h>
#include "sessionManager/contracts/IResponseSink.h"
namespace {

class CollectingSink : public IResponseSink {
public:
    void onEvent(const generation::GenerationEvent& event) override {
        events.push_back(event);
    }

    void onClose() override {
        closed = true;
    }

    bool isValid() const override {
        return !closed;
    }

    std::string getSinkType() const override {
        return "CollectingSink";
    }

    std::vector<generation::GenerationEvent> events;
    bool closed = false;
};

}

DROGON_TEST(GenerationServiceEmitContract_CollectTextDelta)
{
    CollectingSink sink;

    generation::OutputTextDelta delta;
    delta.delta = "abc";
    sink.onEvent(delta);

    CHECK(sink.events.size() == 1);
    CHECK(std::holds_alternative<generation::OutputTextDelta>(sink.events[0]));
}

DROGON_TEST(GenerationServiceEmitContract_CollectToolCall)
{
    CollectingSink sink;

    generation::ToolCallDone tc;
    tc.id = "call_1";
    tc.name = "read_file";
    tc.arguments = "{}";
    sink.onEvent(tc);

    CHECK(sink.events.size() == 1);
    CHECK(std::holds_alternative<generation::ToolCallDone>(sink.events[0]));
}

DROGON_TEST(GenerationServiceEmitContract_CollectCompleted)
{
    CollectingSink sink;

    generation::Completed completed;
    completed.finishReason = "stop";
    sink.onEvent(completed);

    CHECK(sink.events.size() == 1);
    CHECK(std::holds_alternative<generation::Completed>(sink.events[0]));
}

DROGON_TEST(GenerationServiceEmitContract_CollectError)
{
    CollectingSink sink;

    generation::Error error;
    error.code = generation::ErrorCode::ProviderError;
    error.message = "provider failed";
    sink.onEvent(error);

    CHECK(sink.events.size() == 1);
    CHECK(std::holds_alternative<generation::Error>(sink.events[0]));
}

DROGON_TEST(GenerationServiceEmitContract_CloseSemantics)
{
    CollectingSink sink;
    CHECK(sink.isValid());

    sink.onClose();

    CHECK(!sink.isValid());
}
