#include <drogon/drogon_test.h>
#include "sessionManager/tooling/ToolCallNormalizer.h"
using namespace toolcall;

namespace {

generation::ToolCallDone makeCall(const std::string& args) {
    generation::ToolCallDone tc;
    tc.id = "call_1";
    tc.name = "tool";
    tc.arguments = args;
    return tc;
}

}

DROGON_TEST(ToolCallNormalizer_EmptyArgs_ToObject)
{
    session_st session;
    std::vector<generation::ToolCallDone> calls{makeCall("")};

    normalizeToolCallArguments(session, calls);

    CHECK(calls[0].arguments == "{}");
}

DROGON_TEST(ToolCallNormalizer_InvalidJson_ToObject)
{
    session_st session;
    std::vector<generation::ToolCallDone> calls{makeCall("{invalid")};

    normalizeToolCallArguments(session, calls);

    CHECK(calls[0].arguments == "{}");
}

DROGON_TEST(ToolCallNormalizer_NonObject_Wrapped)
{
    session_st session;
    std::vector<generation::ToolCallDone> calls{makeCall("[1,2,3]")};

    normalizeToolCallArguments(session, calls);

    CHECK(calls[0].arguments.find("value") != std::string::npos);
}

DROGON_TEST(ToolCallNormalizer_Object_KeepKeys)
{
    session_st session;
    std::vector<generation::ToolCallDone> calls{makeCall(R"({"path":"a.txt"})")};

    normalizeToolCallArguments(session, calls);

    CHECK(calls[0].arguments.find("path") != std::string::npos);
    CHECK(calls[0].arguments.find("a.txt") != std::string::npos);
}

DROGON_TEST(ToolCallNormalizer_MultipleCalls_AllNormalized)
{
    session_st session;
    std::vector<generation::ToolCallDone> calls;
    calls.push_back(makeCall(""));
    calls.push_back(makeCall("[1]"));
    calls.push_back(makeCall(R"({"ok":true})"));

    normalizeToolCallArguments(session, calls);

    CHECK(calls.size() == 3);
    CHECK(calls[0].arguments == "{}");
    CHECK(calls[1].arguments.find("value") != std::string::npos);
    CHECK(calls[2].arguments.find("ok") != std::string::npos);
}
