#ifndef DBMANAGER_ERROR_EVENT_JSON_ENCODER_H
#define DBMANAGER_ERROR_EVENT_JSON_ENCODER_H

#include <domain/model/ErrorEvent.h>
#include <json/json.h>

#include <chrono>
#include <string>
#include <type_traits>
#include <variant>

// 出站方向 codec：ErrorDetails/ErrorEvent -> Json::Value/紧凑字符串。
// 刻意与 metrics/ErrorEventJsonDecoder.h 分离，理由同该文件注释。
namespace dbmanager::erroreventcodec {

inline std::string compactJson(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

inline Json::Value detailsToJson(const metrics::ErrorDetails& details)
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

inline Json::Value toJson(const metrics::ErrorEvent& event)
{
    Json::Value json(Json::objectValue);
    json["id"] = Json::Int64(event.id);
    json["ts"] = Json::Int64(std::chrono::duration_cast<std::chrono::milliseconds>(
        event.ts.time_since_epoch()).count());
    json["severity"] = metrics::ErrorEvent::severityToString(event.severity);
    json["domain"] = metrics::ErrorEvent::domainToString(event.domain);
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

}  // namespace dbmanager::erroreventcodec

#endif  // DBMANAGER_ERROR_EVENT_JSON_ENCODER_H
