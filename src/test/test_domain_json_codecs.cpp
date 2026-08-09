#include <drogon/drogon_test.h>

#include <controllers/codecs/ChannelJsonCodec.h>

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
