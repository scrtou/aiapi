#include <drogon/drogon_test.h>
#include <application/generation/tooling/StrictClientRules.h>
using namespace toolcall;

namespace {

Json::Value makeFunctionTool(const std::string& name)
{
    Json::Value tool(Json::objectValue);
    tool["type"] = "function";
    tool["function"]["name"] = name;
    tool["function"]["parameters"]["type"] = "object";
    return tool;
}

}

DROGON_TEST(StrictClientRules_HasToolNamed)
{
    Json::Value tools(Json::arrayValue);
    tools.append(makeFunctionTool("read_file"));
    tools.append(makeFunctionTool("apply_diff"));

    CHECK(hasToolNamed(tools, "apply_diff"));
    CHECK(!hasToolNamed(tools, "write_to_file"));
}

DROGON_TEST(StrictClientRules_DetectApplyDiffFailureContext)
{
    Json::Value history(Json::arrayValue);
    Json::Value message(Json::objectValue);
    message["role"] = "user";
    message["content"] = Json::Value(Json::arrayValue);

    Json::Value textPart(Json::objectValue);
    textPart["type"] = "text";
    textPart["text"] = "No sufficiently similar match found at line: 42\nBest Match Found:";
    message["content"].append(textPart);
    history.append(message);

    CHECK(hasApplyDiffFailureContext(history, "", ""));
    CHECK(hasApplyDiffFailureContext(Json::Value(Json::arrayValue),
                                     "But unable to apply all diff parts to file",
                                     ""));
    CHECK(!hasApplyDiffFailureContext(Json::Value(Json::arrayValue),
                                      "ordinary tool output",
                                      ""));
}

DROGON_TEST(StrictClientRules_ReadFileClearsPendingApplyDiffRecovery)
{
    Json::Value history(Json::arrayValue);

    Json::Value failed(Json::objectValue);
    failed["role"] = "tool";
    failed["content"] = "No sufficiently similar match found at line: 42";
    history.append(failed);

    Json::Value readResult(Json::objectValue);
    readResult["role"] = "tool";
    readResult["content"] = "File: README.md\n1 | # Project\n2 | content";
    history.append(readResult);

    CHECK(!hasApplyDiffFailureContext(history, "", ""));

    Json::Value failedAgain(Json::objectValue);
    failedAgain["role"] = "tool";
    failedAgain["content"] = "But unable to apply all diff parts to file";
    history.append(failedAgain);

    CHECK(hasApplyDiffFailureContext(history, "", ""));
}

DROGON_TEST(StrictClientRules_ApplyDiffPolicyExactMatch)
{
    const std::string policy = buildStrictApplyDiffPolicy(false);

    CHECK(policy.find("100% exact match") != std::string::npos);
    CHECK(policy.find("case-sensitive") != std::string::npos);
    CHECK(policy.find("never use ---/+++ Unified Diff") != std::string::npos);
    CHECK(policy.find("Do NOT resend") == std::string::npos);
}

DROGON_TEST(StrictClientRules_ApplyDiffRecoveryPolicy)
{
    const std::string policy = buildStrictApplyDiffPolicy(true);

    CHECK(policy.find("Do NOT resend the same path+diff payload") != std::string::npos);
    CHECK(policy.find("read_file the smallest affected range") != std::string::npos);
    CHECK(policy.find("Best Match Found") != std::string::npos);
    CHECK(policy.find("operation: modified") != std::string::npos);
}

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
