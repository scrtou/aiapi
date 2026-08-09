#include <drogon/drogon_test.h>

#include <accountManager/AccountJsonCodec.h>
#include <controllers/codecs/ChannelJsonCodec.h>

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
