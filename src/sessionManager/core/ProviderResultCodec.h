#ifndef PROVIDER_RESULT_CODEC_H
#define PROVIDER_RESULT_CODEC_H

#include <domain/model/ProviderResult.h>
#include <json/json.h>

// JSON is an edge concern.  This adapter is kept next to the legacy
// generation pipeline until that pipeline moves into aiapi_application.
namespace providercodec {

inline Json::Value toJson(const provider::ProviderMetadata& metadata)
{
    Json::Value result(Json::objectValue);
    for (const auto& entry : metadata)
    {
        result[entry.first] = entry.second;
    }
    return result;
}

inline provider::ProviderMetadata fromJson(const Json::Value& value)
{
    provider::ProviderMetadata result;
    if (!value.isObject()) return result;
    for (const auto& key : value.getMemberNames())
    {
        if (value[key].isString()) result.emplace(key, value[key].asString());
    }
    return result;
}

}  // namespace providercodec

#endif  // PROVIDER_RESULT_CODEC_H
