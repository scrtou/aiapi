#include "sessionManager/core/RetiredProviderTelemetry.h"

#include <domain/model/ErrorEvent.h>
#include <metrics/ErrorStatsService.h>

namespace observability {

void recordRetiredProviderRoute(const std::string& requestId,
                                int httpStatus,
                                const Json::Value& detail)
{
    metrics::ErrorStatsService::getInstance().recordWarn(
        metrics::Domain::REQUEST,
        "provider.retired_route_called",
        "A retired provider route was called",
        requestId,
        "",  // Intentionally empty: this is not an active provider request.
        "", "", "", false, httpStatus, detail);
}

}  // namespace observability
