#include <drogon/drogon_test.h>

#include <application/metrics/MetricsUseCase.h>
#include <domain/port/IMetricsQuery.h>

// ARCH_TESTS: application/metrics/MetricsUseCase.h
// ARCH_TESTS: domain/port/IMetricsUseCase.h

namespace {

class ErrorQuery final : public metrics::IErrorMetricsQuery
{
  public:
    int requestCalls = 0;
    int eventCalls = 0;

    std::vector<metrics::AggBucket> queryErrorSeries(
        const metrics::QueryParams&) override { return {}; }
    std::vector<metrics::AggBucket> queryRequestSeries(
        const metrics::QueryParams&) override
    {
        ++requestCalls;
        return {{"bucket", 3}};
    }
    std::vector<metrics::ErrorEventRecord> queryEvents(
        const metrics::QueryParams&, int, int) override
    {
        ++eventCalls;
        return {};
    }
    std::optional<metrics::ErrorEventRecord> queryEventById(std::int64_t) override
    { return std::nullopt; }
};

class StatusQuery final : public metrics::IStatusMetricsQuery
{
  public:
    int summaryCalls = 0;

    metrics::StatusSummaryData getStatusSummary(
        const metrics::StatusQueryParams&) override
    {
        ++summaryCalls;
        metrics::StatusSummaryData result;
        result.totalRequests = 9;
        return result;
    }
    std::vector<metrics::ChannelStatusData> getChannelStatusList(
        const metrics::StatusQueryParams&) override { return {}; }
    std::vector<metrics::ModelStatusData> getModelStatusList(
        const metrics::StatusQueryParams&) override { return {}; }
};

}  // namespace

DROGON_TEST(MetricsUseCaseForwardsInjectedReadPorts)
{
    ErrorQuery errors;
    StatusQuery status;
    metrics::MetricsUseCase useCase(&errors, &status);

    const auto requests = useCase.requestSeries({});
    const auto summary = useCase.statusSummary({});
    const auto events = useCase.errorEvents({}, 10, 0);

    REQUIRE(requests.size() == 1);
    CHECK(requests.front().count == 3);
    CHECK(summary.totalRequests == 9);
    CHECK(events.empty());
    CHECK(errors.requestCalls == 1);
    CHECK(errors.eventCalls == 1);
    CHECK(status.summaryCalls == 1);
}
