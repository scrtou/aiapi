#ifndef CHANNEL_JSON_CODEC_H
#define CHANNEL_JSON_CODEC_H

#include <domain/model/ChannelInfo.h>
#include <json/json.h>

namespace channelcodec {

inline Channelinfo_st fromJson(const Json::Value& value)
{
    Channelinfo_st channel;
    channel.id = value.get("id", 0).asInt();
    channel.channelName = value.get("channelname", "").asString();
    channel.channelType = value.get("channeltype", "").asString();
    channel.channelUrl = value.get("channelurl", "").asString();
    channel.channelKey = value.get("channelkey", "").asString();
    channel.channelStatus = value.get("channelstatus", true).asBool();
    channel.maxConcurrent = value.get("maxconcurrent", 10).asInt();
    channel.timeout = value.get("timeout", 30).asInt();
    channel.priority = value.get("priority", 0).asInt();
    channel.description = value.get("description", "").asString();
    channel.createTime = value.get("createtime", "").asString();
    channel.updateTime = value.get("updatetime", "").asString();
    channel.accountCount = value.get("accountcount", 0).asInt();
    channel.accountRetentionDays = value.get("accountretentiondays", 0).asInt();
    channel.supportsToolCalls = value.get("supports_tool_calls", false).asBool();
    return channel;
}

inline Json::Value toJson(const Channelinfo_st& channel, bool includeSecret = false)
{
    Json::Value value(Json::objectValue);
    value["id"] = channel.id;
    value["channelname"] = channel.channelName;
    value["channeltype"] = channel.channelType;
    value["channelurl"] = channel.channelUrl;
    value["channelstatus"] = channel.channelStatus;
    value["maxconcurrent"] = channel.maxConcurrent;
    value["timeout"] = channel.timeout;
    value["priority"] = channel.priority;
    value["description"] = channel.description;
    value["createtime"] = channel.createTime;
    value["updatetime"] = channel.updateTime;
    value["accountcount"] = channel.accountCount;
    value["accountretentiondays"] = channel.accountRetentionDays;
    value["supports_tool_calls"] = channel.supportsToolCalls;
    if (includeSecret) value["channelkey"] = channel.channelKey;
    return value;
}

}  // namespace channelcodec

#endif  // CHANNEL_JSON_CODEC_H
