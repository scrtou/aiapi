#include <drogon/drogon_test.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <controllers/MetricsController.h>

// ARCH_TESTS: domain/model/MetricsData.h
// ARCH_TESTS: domain/port/IMetricsUseCase.h

namespace {

class FakeMetricsUseCase final : public metrics::IMetricsUseCase
{
  public:
    int requestSeriesCalls = 0;
    int summaryCalls = 0;

    std::vector<metrics::AggBucket> requestSeries(
        const metrics::QueryParams&) override
    {
        ++requestSeriesCalls;
        return {{"2026-08-13T00:00:00Z", 7}};
    }
    std::vector<metrics::AggBucket> errorSeries(
        const metrics::QueryParams&) override { return {}; }
    std::vector<metrics::ErrorEventRecord> errorEvents(
        const metrics::QueryParams&, int, int) override { return {}; }
    std::optional<metrics::ErrorEventRecord> errorEventById(std::int64_t) override
    { return std::nullopt; }
    metrics::StatusSummaryData statusSummary(
        const metrics::StatusQueryParams&) override
    {
        ++summaryCalls;
        metrics::StatusSummaryData result;
        result.totalRequests = 11;
        result.totalErrors = 2;
        result.overallStatus = metrics::ServiceHealthStatus::DEGRADED;
        return result;
    }
    std::vector<metrics::ChannelStatusData> channelStatus(
        const metrics::StatusQueryParams&) override { return {}; }
    std::vector<metrics::ModelStatusData> modelStatus(
        const metrics::StatusQueryParams&) override { return {}; }
};

}  // namespace

DROGON_TEST(MetricsControllerUsesInjectedUseCaseForErrorQueries)
{
    FakeMetricsUseCase metrics;
    MetricsController::setUseCase(&metrics);

    drogon::HttpResponsePtr captured;
    MetricsController controller;
    controller.getRequestsSeries(
        drogon::HttpRequest::newHttpRequest(),
        [&captured](const drogon::HttpResponsePtr& response) { captured = response; });

    REQUIRE(captured != nullptr);
    const auto json = captured->getJsonObject();
    REQUIRE(json != nullptr);
    CHECK(metrics.requestSeriesCalls == 1);
    CHECK((*json)["data"].size() == 1);
    CHECK((*json)["data"][0]["count"].asInt64() == 7);

    MetricsController::setUseCase(nullptr);
}

DROGON_TEST(MetricsControllerUsesInjectedUseCaseForStatusQueries)
{
    FakeMetricsUseCase metrics;
    MetricsController::setUseCase(&metrics);

    drogon::HttpResponsePtr captured;
    MetricsController controller;
    controller.getStatusSummary(
        drogon::HttpRequest::newHttpRequest(),
        [&captured](const drogon::HttpResponsePtr& response) { captured = response; });

    REQUIRE(captured != nullptr);
    const auto json = captured->getJsonObject();
    REQUIRE(json != nullptr);
    CHECK(metrics.summaryCalls == 1);
    CHECK((*json)["total_requests"].asInt64() == 11);
    CHECK((*json)["overall_status"].asString() == "DEGRADED");

    MetricsController::setUseCase(nullptr);
}
