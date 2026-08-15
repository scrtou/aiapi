#pragma once

#include <json/json.h>
#include <domain/port/ITelemetrySink.h>

#include <string>

namespace observability {

void setTelemetrySink(metrics::ITelemetrySink* sink);

/// Record a retired-route call without reintroducing its provider key into
/// active request/status dimensions.
void recordRetiredProviderRoute(const std::string& requestId,
                                int httpStatus,
                                const Json::Value& detail);

}  // namespace observability
