#pragma once

#include <domain/port/IMetricsUseCase.h>

namespace metrics {
class IErrorMetricsQuery;
class IStatusMetricsQuery;

class MetricsUseCase final : public IMetricsUseCase
{
  public:
    MetricsUseCase(IErrorMetricsQuery* errors, IStatusMetricsQuery* status);

    std::vector<AggBucket> requestSeries(const QueryParams& params) override;
    std::vector<AggBucket> errorSeries(const QueryParams& params) override;
    std::vector<ErrorEventRecord> errorEvents(
        const QueryParams& params, int limit, int offset) override;
    std::optional<ErrorEventRecord> errorEventById(std::int64_t id) override;
    StatusSummaryData statusSummary(const StatusQueryParams& params) override;
    std::vector<ChannelStatusData> channelStatus(
        const StatusQueryParams& params) override;
    std::vector<ModelStatusData> modelStatus(
        const StatusQueryParams& params) override;

  private:
    IErrorMetricsQuery* errors_;
    IStatusMetricsQuery* status_;
};

}  // namespace metrics
