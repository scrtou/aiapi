#include <drogon/drogon_test.h>

#include <transport/controllers/codecs/AccountJsonCodec.h>
#include <transport/controllers/codecs/ChannelJsonCodec.h>
#include <infrastructure/codec/RetoolWorkspaceJsonCodec.h>

DROGON_TEST(AccountJsonCodec_RoundTripsCurrentWireFields)
{
    Json::Value input(Json::objectValue);
    input["apiName"] = "chaynsapi";
    input["userName"] = "user@example.invalid";
    input["password"] = "secret-password";
    input["authToken"] = "secret-token";
    input["useCount"] = 4;
    input["tokenStatus"] = true;
    input["accountStatus"] = true;
    input["userTobitId"] = 7;
    input["personId"] = "person-1";
    input["createTime"] = "2026-08-09";
    input["accountType"] = "pro";
    input["status"] = "active";
    input["workspaceUacId"] = Json::Int64(42);

    const auto account = accountcodec::fromJson(input);
    const Json::Value output = accountcodec::toJson(account, true);
    CHECK(output == input);
}

DROGON_TEST(AccountJsonCodec_DefaultsUnknownFieldsAndRedactsSecrets)
{
    Json::Value input(Json::objectValue);
    input["unknown"] = "ignored";
    input["workspaceUacId"] = Json::Int64(-7);

    const auto account = accountcodec::fromJson(input);
    CHECK(account.accountType == "free");
    CHECK(account.status == "active");
    CHECK(account.workspaceUacId == 0);

    const Json::Value output = accountcodec::toJson(account);
    CHECK(!output.isMember("password"));
    CHECK(!output.isMember("authToken"));
    CHECK(!output.isMember("unknown"));
}

DROGON_TEST(ChannelJsonCodec_RoundTripsCurrentWireFields)
{
    Json::Value input(Json::objectValue);
    input["id"] = 3;
    input["channelname"] = "chaynsapi";
    input["channeltype"] = "provider";
    input["channelurl"] = "https://example.invalid";
    input["channelkey"] = "secret-key";
    input["channelstatus"] = true;
    input["maxconcurrent"] = 12;
    input["timeout"] = 45;
    input["priority"] = 2;
    input["description"] = "fixture";
    input["createtime"] = "created";
    input["updatetime"] = "updated";
    input["accountcount"] = 8;
    input["accountretentiondays"] = 6;
    input["supports_tool_calls"] = true;

    const auto channel = channelcodec::fromJson(input);
    const Json::Value output = channelcodec::toJson(channel, true);
    CHECK(output == input);
}

DROGON_TEST(ChannelJsonCodec_DefaultsUnknownFieldsAndRedactsSecret)
{
    Json::Value input(Json::objectValue);
    input["unknown"] = "ignored";
    input["channelkey"] = "secret-key";

    const auto channel = channelcodec::fromJson(input);
    CHECK(channel.channelStatus);
    CHECK(channel.maxConcurrent == 10);
    CHECK(channel.timeout == 30);
    CHECK(!channel.supportsToolCalls);

    const Json::Value output = channelcodec::toJson(channel);
    CHECK(!output.isMember("channelkey"));
    CHECK(!output.isMember("unknown"));
}

DROGON_TEST(RetoolWorkspaceJsonCodec_RoundTripsNestedFields)
{
    Json::Value input(Json::objectValue);
    input["workspaceId"] = "ws-1";
    input["email"] = "owner@example.invalid";
    input["password"] = "password";
    input["mailProvider"] = "fixture-mail";
    input["mailAccountId"] = "mail-1";
    input["baseUrl"] = "https://workspace.invalid";
    input["subdomain"] = "workspace";
    input["accessToken"] = "access-token";
    input["xsrfToken"] = "xsrf-token";
    input["extraCookies"]["session"] = "cookie";
    input["openaiResourceUuid"] = "openai-uuid";
    input["openaiResourceName"] = "openai-name";
    input["anthropicResourceUuid"] = "anthropic-uuid";
    input["anthropicResourceName"] = "anthropic-name";
    input["workflowId"] = "workflow-1";
    input["workflowApiKey"] = "workflow-key";
    input["agentId"] = "agent-1";
    input["status"] = "ready";
    input["verifyStatus"] = "passed";
    input["lastVerifyAt"] = "verified";
    input["lastUsedAt"] = "used";
    input["inUseCount"] = 2;
    input["notes"]["owner"] = "test";
    input["createdAt"] = "created";
    input["updatedAt"] = "updated";

    const auto workspace = retoolworkspacecodec::fromJson(input);
    const Json::Value output = retoolworkspacecodec::toJson(workspace, true);
    CHECK(output == input);
}

DROGON_TEST(RetoolWorkspaceJsonCodec_AcceptsSnakeCaseAndRedactsSecrets)
{
    Json::Value input(Json::objectValue);
    input["workspace_id"] = "ws-snake";
    input["base_url"] = "https://snake.invalid";
    input["access_token"] = "access-token";
    input["xsrf_token"] = "xsrf-token";
    input["in_use_count"] = 3;
    input["unknown"] = "ignored";

    const auto workspace = retoolworkspacecodec::fromJson(input);
    CHECK(workspace.workspaceId == "ws-snake");
    CHECK(workspace.baseUrl == "https://snake.invalid");
    CHECK(workspace.inUseCount == 3);
    CHECK(workspace.status == "provisioning");
    CHECK(workspace.verifyStatus == "unknown");

    const Json::Value output = retoolworkspacecodec::toJson(workspace);
    CHECK(!output.isMember("password"));
    CHECK(!output.isMember("accessToken"));
    CHECK(!output.isMember("xsrfToken"));
    CHECK(!output.isMember("unknown"));
}
