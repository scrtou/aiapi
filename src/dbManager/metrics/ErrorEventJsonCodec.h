#ifndef ERROR_EVENT_JSON_CODEC_H
#define ERROR_EVENT_JSON_CODEC_H

#include <domain/model/ErrorEvent.h>
#include <json/json.h>

#include <sstream>
#include <type_traits>

namespace metrics::erroreventcodec {

inline std::string compactJson(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

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
            details.emplace(key, compactJson(item));
    }
    return details;
}

inline Json::Value detailsToJson(const ErrorDetails& details)
{
    Json::Value result(Json::objectValue);
    for (const auto& entry : details)
    {
        std::visit([&result, &entry](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::int64_t>)
                result[entry.first] = Json::Int64(value);
            else
                result[entry.first] = value;
        }, entry.second);
    }
    return result;
}

inline Json::Value toJson(const ErrorEvent& event)
{
    Json::Value json(Json::objectValue);
    json["id"] = Json::Int64(event.id);
    json["ts"] = Json::Int64(std::chrono::duration_cast<std::chrono::milliseconds>(
        event.ts.time_since_epoch()).count());
    json["severity"] = ErrorEvent::severityToString(event.severity);
    json["domain"] = ErrorEvent::domainToString(event.domain);
    json["type"] = event.type;
    json["provider"] = event.provider;
    json["model"] = event.model;
    json["client_type"] = event.clientType;
    json["api_kind"] = event.apiKind;
    json["stream"] = event.stream;
    json["http_status"] = event.httpStatus;
    json["request_id"] = event.requestId;
    json["response_id"] = event.responseId;
    json["tool_name"] = event.toolName;
    json["message"] = event.message;
    json["detail_json"] = detailsToJson(event.details);
    json["raw_snippet"] = event.rawSnippet;
    return json;
}

}  // namespace metrics::erroreventcodec

#endif  // ERROR_EVENT_JSON_CODEC_H
