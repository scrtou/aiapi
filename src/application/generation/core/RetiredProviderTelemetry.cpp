#include <application/generation/core/RetiredProviderTelemetry.h>

#include <domain/model/ErrorEvent.h>

namespace observability {

namespace {
metrics::ITelemetrySink* telemetrySink = nullptr;
}

void setTelemetrySink(metrics::ITelemetrySink* sink) { telemetrySink = sink; }

void recordRetiredProviderRoute(const std::string& requestId,
                                int httpStatus,
                                const Json::Value& detail)
{
    if (!telemetrySink) return;
    metrics::ErrorEvent event;
    event.ts = std::chrono::system_clock::now();
    event.severity = metrics::Severity::WARN;
    event.domain = metrics::Domain::REQUEST;
    event.type = "provider.retired_route_called";
    event.message = "A retired provider route was called";
    event.requestId = requestId;
    event.httpStatus = httpStatus;
    if (detail.isObject()) {
        for (const auto& key : detail.getMemberNames()) {
            const auto& value = detail[key];
            if (value.isString()) event.details[key] = value.asString();
            else if (value.isBool()) event.details[key] = value.asBool();
            else if (value.isInt64()) event.details[key] = value.asInt64();
            else if (value.isUInt64())
                event.details[key] = static_cast<std::int64_t>(value.asUInt64());
            else if (value.isDouble()) event.details[key] = value.asDouble();
        }
    }
    telemetrySink->record(event);
}

}  // namespace observability
