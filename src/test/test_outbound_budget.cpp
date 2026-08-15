#include <drogon/drogon_test.h>
#include <infrastructure/provider/limits/OutboundBudget.h>

using namespace continuity;

DROGON_TEST(OutboundBudget_ProvidersHaveIndependentLimits)
{
    const size_t chaynsLimit = outboundMaxRequestBytes("chaynsapi");
    const size_t retoolLimit = outboundMaxRequestBytes("retoolapi");
    const size_t genericLimit = outboundMaxRequestBytes("test-provider");

    CHECK(chaynsLimit > 0);
    CHECK(retoolLimit > 0);
    CHECK(genericLimit > 0);
    CHECK(chaynsLimit != genericLimit);
    CHECK(genericLimit != retoolLimit);
}

DROGON_TEST(OutboundBudget_UnknownProviderUsesGenericFallback)
{
    const size_t unknownA = outboundMaxRequestBytes("does-not-exist");
    const size_t unknownB = outboundMaxRequestBytes("another-unknown");
    CHECK(unknownA > 0);
    CHECK(unknownA == unknownB);
}

DROGON_TEST(OutboundBudget_MessageLimitBelowRequestLimit)
{
    for (const char* provider : {"chaynsapi", "retoolapi", "test-provider"}) {
        CHECK(outboundMaxMessageBytes(provider) <= outboundMaxRequestBytes(provider));
    }
}

DROGON_TEST(OutboundBudget_LadderHalvesAndEndsAtZero)
{
    const auto ladder = degradationLadder(800000);
    REQUIRE(ladder.size() >= 2);
    CHECK(ladder.front() == 800000);
    CHECK(ladder.back() == 0);
    for (size_t i = 1; i + 1 < ladder.size(); ++i) {
        CHECK(ladder[i] == ladder[i - 1] / 2);
    }
}

DROGON_TEST(OutboundBudget_LadderFromZeroIsSingleStep)
{
    const auto ladder = degradationLadder(0);
    REQUIRE(ladder.size() == 1);
    CHECK(ladder[0] == 0);
}

DROGON_TEST(OutboundBudget_LadderRespectsStepCount)
{
    const auto ladder = degradationLadder(1024, 3);
    REQUIRE(ladder.size() == 4);
    CHECK(ladder[0] == 1024);
    CHECK(ladder[1] == 512);
    CHECK(ladder[2] == 256);
    CHECK(ladder[3] == 0);
}

DROGON_TEST(OutboundBudget_CompactBodyBytesMatchesCompactSerialization)
{
    Json::Value body(Json::objectValue);
    body["text"] = "hello";

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    CHECK(compactBodyBytes(body) == Json::writeString(writer, body).size());
}

DROGON_TEST(OutboundBudget_CheckDetectsOversizeBody)
{
    const size_t limit = outboundMaxRequestBytes("chaynsapi");
    REQUIRE(limit > 0);

    const auto small = checkOutboundSize("chaynsapi", limit - 1);
    CHECK(small.withinLimit);
    CHECK(small.limitBytes == limit);
    CHECK(small.actualBytes == limit - 1);

    const auto exact = checkOutboundSize("chaynsapi", limit);
    CHECK(exact.withinLimit);

    const auto oversize = checkOutboundSize("chaynsapi", limit + 1);
    CHECK(!oversize.withinLimit);
    CHECK(oversize.actualBytes == limit + 1);
}

DROGON_TEST(OutboundBudget_CheckAcceptsJsonBody)
{
    Json::Value body(Json::objectValue);
    body["text"] = std::string(64, 'x');

    const auto check = checkOutboundSize("chaynsapi", body);
    CHECK(check.withinLimit);
    CHECK(check.actualBytes == compactBodyBytes(body));
}
