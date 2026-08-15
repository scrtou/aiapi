#ifndef METRICS_ERROR_EVENT_JSON_DECODER_H
#define METRICS_ERROR_EVENT_JSON_DECODER_H

#include <domain/model/ErrorEvent.h>
#include <json/json.h>

#include <cstdint>

// 入站方向 codec：Json::Value -> ErrorDetails。
// 刻意与 infrastructure/persistence/metrics/ErrorEventJsonEncoder.h 分离：两侧各自只依赖
// domain/model + JsonCpp，互不引用，从而不产生 metrics <-> dbManager 跨模块边。
// 下方 compactJson 与 encoder 侧同名实现重复约 5 行，属于有意为之，
// 合并会立刻重新引入越界依赖，请勿「优化」。
namespace metrics::erroreventcodec {

namespace detail {

inline std::string compactJson(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

}  // namespace detail

inline ErrorDetails detailsFromJson(const Json::Value& value)
{
    ErrorDetails details;
    if (!value.isObject()) return details;
    for (const auto& key : value.getMemberNames())
    {
        const auto& item = value[key];
        if (item.isString())
            details.emplace(key, item.asString());
        else if (item.isBool())
            details.emplace(key, item.asBool());
        else if (item.isInt64() || item.isUInt64())
            details.emplace(key, static_cast<std::int64_t>(item.asLargestInt()));
        else if (item.isDouble())
            details.emplace(key, item.asDouble());
        else
            details.emplace(key, detail::compactJson(item));
    }
    return details;
}

}  // namespace metrics::erroreventcodec

#endif  // METRICS_ERROR_EVENT_JSON_DECODER_H
