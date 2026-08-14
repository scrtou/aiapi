#pragma once

#include <domain/model/MetricsData.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace metrics {

class IErrorMetricsQuery
{
  public:
    virtual ~IErrorMetricsQuery() = default;
    virtual std::vector<AggBucket> queryErrorSeries(const QueryParams& params) = 0;
    virtual std::vector<AggBucket> queryRequestSeries(const QueryParams& params) = 0;
    virtual std::vector<ErrorEventRecord> queryEvents(
        const QueryParams& params, int limit, int offset) = 0;
    virtual std::optional<ErrorEventRecord> queryEventById(std::int64_t id) = 0;
};

class IStatusMetricsQuery
{
  public:
    virtual ~IStatusMetricsQuery() = default;
    virtual StatusSummaryData getStatusSummary(const StatusQueryParams& params) = 0;
    virtual std::vector<ChannelStatusData> getChannelStatusList(
        const StatusQueryParams& params) = 0;
    virtual std::vector<ModelStatusData> getModelStatusList(
        const StatusQueryParams& params) = 0;
};

}  // namespace metrics
