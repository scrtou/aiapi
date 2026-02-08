#include <drogon/drogon_test.h>
#include "sessionManager/tooling/StrictClientRules.h"
using namespace toolcall;

DROGON_TEST(StrictClientRules_TextOnly_WrapAttemptCompletion)
{
    std::vector<generation::ToolCallDone> toolCalls;
    std::string text = "final answer";

    applyStrictClientRules("Kilo-Code", text, toolCalls);

    CHECK(toolCalls.size() == 1);
    CHECK(toolCalls[0].name == "attempt_completion");
    CHECK(text.empty());
}

DROGON_TEST(StrictClientRules_OneToolCall_KeepSingle)
{
    std::vector<generation::ToolCallDone> toolCalls;
    generation::ToolCallDone tc;
    tc.id = "call_1";
    tc.name = "read_file";
    tc.arguments = R"({"path":"a.txt"})";
    toolCalls.push_back(tc);

    std::string text = "";
    applyStrictClientRules("RooCode", text, toolCalls);

    CHECK(toolCalls.size() == 1);
    CHECK(toolCalls[0].name == "read_file");
}

DROGON_TEST(StrictClientRules_MultiToolCalls_TruncateToOne)
{
    std::vector<generation::ToolCallDone> toolCalls;

    generation::ToolCallDone tc1;
    tc1.id = "call_1";
    tc1.name = "read_file";
    tc1.arguments = "{}";
    toolCalls.push_back(tc1);

    generation::ToolCallDone tc2;
    tc2.id = "call_2";
    tc2.name = "write_to_file";
    tc2.arguments = "{}";
    toolCalls.push_back(tc2);

    std::string text;
    applyStrictClientRules("Kilo-Code", text, toolCalls);

    CHECK(toolCalls.size() == 1);
    CHECK(toolCalls[0].id == "call_1");
}

DROGON_TEST(StrictClientRules_EmptyTextEmptyCalls_NoWrap)
{
    std::vector<generation::ToolCallDone> toolCalls;
    std::string text;

    applyStrictClientRules("Kilo-Code", text, toolCalls);

    CHECK(toolCalls.empty());
    CHECK(text.empty());
}

DROGON_TEST(StrictClientRules_NonStrictName_StillDeterministic)
{
    std::vector<generation::ToolCallDone> toolCalls;
    std::string text = "hello";

    applyStrictClientRules("OtherClient", text, toolCalls);

    CHECK(toolCalls.size() == 1);
    CHECK(toolCalls[0].name == "attempt_completion");
}
