#pragma once

#include <json/json.h>

#include <string>

namespace observability {

/// Record a retired-route call without reintroducing its provider key into
/// active request/status dimensions.
void recordRetiredProviderRoute(const std::string& requestId,
                                int httpStatus,
                                const Json::Value& detail);

}  // namespace observability
