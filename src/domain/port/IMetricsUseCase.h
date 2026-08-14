#pragma once

#include <domain/model/MetricsData.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace metrics {

/** Controller-facing read workflow for metrics. */
class IMetricsUseCase
{
  public:
    virtual ~IMetricsUseCase() = default;

    virtual std::vector<AggBucket> requestSeries(const QueryParams& params) = 0;
    virtual std::vector<AggBucket> errorSeries(const QueryParams& params) = 0;
    virtual std::vector<ErrorEventRecord> errorEvents(
        const QueryParams& params, int limit, int offset) = 0;
    virtual std::optional<ErrorEventRecord> errorEventById(std::int64_t id) = 0;
    virtual StatusSummaryData statusSummary(const StatusQueryParams& params) = 0;
    virtual std::vector<ChannelStatusData> channelStatus(
        const StatusQueryParams& params) = 0;
    virtual std::vector<ModelStatusData> modelStatus(
        const StatusQueryParams& params) = 0;
};

}  // namespace metrics
