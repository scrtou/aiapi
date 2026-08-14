#include <application/metrics/MetricsUseCase.h>

#include <domain/port/IMetricsQuery.h>

namespace metrics {

MetricsUseCase::MetricsUseCase(IErrorMetricsQuery* errors, IStatusMetricsQuery* status)
    : errors_(errors), status_(status)
{
}

std::vector<AggBucket> MetricsUseCase::requestSeries(const QueryParams& params)
{
    return errors_ ? errors_->queryRequestSeries(params) : std::vector<AggBucket>{};
}

std::vector<AggBucket> MetricsUseCase::errorSeries(const QueryParams& params)
{
    return errors_ ? errors_->queryErrorSeries(params) : std::vector<AggBucket>{};
}

std::vector<ErrorEventRecord> MetricsUseCase::errorEvents(
    const QueryParams& params, int limit, int offset)
{
    return errors_ ? errors_->queryEvents(params, limit, offset)
                   : std::vector<ErrorEventRecord>{};
}

std::optional<ErrorEventRecord> MetricsUseCase::errorEventById(std::int64_t id)
{
    return errors_ ? errors_->queryEventById(id) : std::optional<ErrorEventRecord>{};
}

StatusSummaryData MetricsUseCase::statusSummary(const StatusQueryParams& params)
{
    return status_ ? status_->getStatusSummary(params) : StatusSummaryData{};
}

std::vector<ChannelStatusData> MetricsUseCase::channelStatus(
    const StatusQueryParams& params)
{
    return status_ ? status_->getChannelStatusList(params)
                   : std::vector<ChannelStatusData>{};
}

std::vector<ModelStatusData> MetricsUseCase::modelStatus(const StatusQueryParams& params)
{
    return status_ ? status_->getModelStatusList(params)
                   : std::vector<ModelStatusData>{};
}

}  // namespace metrics
