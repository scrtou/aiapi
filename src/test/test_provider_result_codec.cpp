#include <drogon/drogon_test.h>

#include <sessionManager/core/ProviderResultCodec.h>

#include <type_traits>

static_assert(std::is_same_v<provider::ProviderMetadata,
                             std::map<std::string, std::string>>,
              "provider metadata must remain a transport-neutral string map");

DROGON_TEST(ProviderResultCodec_RoundTripsStringMetadata)
{
    const provider::ProviderMetadata input{
        {"provider", "anthropic"},
        {"routeType", "agent"},
        {"workspaceId", "ws-1"},
    };

    const Json::Value json = providercodec::toJson(input);
    CHECK(json.isObject());
    CHECK(json["provider"].asString() == "anthropic");
    CHECK(providercodec::fromJson(json) == input);
}

DROGON_TEST(ProviderResultCodec_EmptyMetadataProducesEmptyObject)
{
    const Json::Value json = providercodec::toJson({});
    CHECK(json.isObject());
    CHECK(json.empty());
    CHECK(providercodec::fromJson(Json::Value(Json::nullValue)).empty());
}

DROGON_TEST(ProviderResultCodec_PreservesUnknownStringsAndRejectsTypedValues)
{
    Json::Value json(Json::objectValue);
    json["future_key"] = "future-value";
    json["typed_number"] = 42;
    json["typed_object"]["nested"] = true;

    const auto metadata = providercodec::fromJson(json);
    CHECK(metadata.size() == 1);
    CHECK(metadata.at("future_key") == "future-value");
    CHECK(metadata.count("typed_number") == 0);
    CHECK(metadata.count("typed_object") == 0);
}
