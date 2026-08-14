#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace metrics {

struct QueryParams {
    std::string from;
    std::string to;
    std::string severity;
    std::string domain;
    std::string type;
    std::string provider;
    std::string model;
    std::string clientType;
    std::string apiKind;
};

struct AggBucket {
    std::string bucketStart;
    std::int64_t count = 0;
};

struct ErrorEventRecord {
    std::int64_t id = 0;
    std::string ts;
    std::string severity;
    std::string domain;
    std::string type;
    std::string provider;
    std::string model;
    std::string clientType;
    std::string apiKind;
    bool stream = false;
    int httpStatus = 0;
    std::string requestId;
    std::string responseId;
    std::string toolName;
    std::string message;
    std::string detailJson;
    std::string rawSnippet;
};

enum class ServiceHealthStatus { OK, DEGRADED, DOWN, UNKNOWN };

inline const char* statusToString(ServiceHealthStatus status)
{
    switch (status) {
        case ServiceHealthStatus::OK: return "OK";
        case ServiceHealthStatus::DEGRADED: return "DEGRADED";
        case ServiceHealthStatus::DOWN: return "DOWN";
        case ServiceHealthStatus::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

struct StatusBucket {
    std::string bucketStart;
    std::int64_t requestCount = 0;
    std::int64_t errorCount = 0;
    double errorRate = 0.0;
};

struct ChannelStatusData {
    std::string channelId;
    std::string channelName;
    std::int64_t totalRequests = 0;
    std::int64_t totalErrors = 0;
    double errorRate = 0.0;
    ServiceHealthStatus status = ServiceHealthStatus::UNKNOWN;
    std::string lastRequestTime;
    std::vector<StatusBucket> buckets;
};

struct ModelStatusData {
    std::string model;
    std::string provider;
    std::int64_t totalRequests = 0;
    std::int64_t totalErrors = 0;
    double errorRate = 0.0;
    ServiceHealthStatus status = ServiceHealthStatus::UNKNOWN;
    std::string lastRequestTime;
    std::vector<StatusBucket> buckets;
};

struct StatusSummaryData {
    std::int64_t totalRequests = 0;
    std::int64_t totalErrors = 0;
    double errorRate = 0.0;
    int channelCount = 0;
    int modelCount = 0;
    int healthyChannels = 0;
    int degradedChannels = 0;
    int downChannels = 0;
    ServiceHealthStatus overallStatus = ServiceHealthStatus::UNKNOWN;
    std::vector<StatusBucket> buckets;
};

struct StatusQueryParams {
    std::string from;
    std::string to;
    std::string provider;
    std::string model;
    int bucketCount = 24;
};

struct ChannelStatusCounts {
    int healthy = 0;
    int degraded = 0;
    int down = 0;
};

}  // namespace metrics
