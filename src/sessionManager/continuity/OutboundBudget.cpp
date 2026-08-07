#include "sessionManager/continuity/OutboundBudget.h"

#include <drogon/drogon.h>
#include <algorithm>
#include <cstdint>

namespace {

// 每个上游的实测硬限不同，这里的兜底值互相独立，不共用一个常量。
constexpr size_t kFallbackChayns   = 768 * 1024;
constexpr size_t kFallbackNexos    = 512 * 1024;
constexpr size_t kFallbackRetool   = 256 * 1024;
constexpr size_t kFallbackOpenAi   = 4 * 1024 * 1024;
constexpr size_t kFallbackGeneric  = 512 * 1024;

constexpr size_t kFallbackMsgChayns  = 384 * 1024;
constexpr size_t kFallbackMsgNexos   = 256 * 1024;
constexpr size_t kFallbackMsgRetool  = 128 * 1024;
constexpr size_t kFallbackMsgOpenAi  = 1024 * 1024;
constexpr size_t kFallbackMsgGeneric = 256 * 1024;

constexpr size_t kMaximumConfiguredBytes = 32 * 1024 * 1024;

size_t builtinRequestFallback(const std::string& providerKey)
{
    if (providerKey == "chaynsapi") return kFallbackChayns;
    if (providerKey == "nexosapi")  return kFallbackNexos;
    if (providerKey == "retoolapi") return kFallbackRetool;
    if (providerKey == "openai")    return kFallbackOpenAi;
    return kFallbackGeneric;
}

size_t builtinMessageFallback(const std::string& providerKey)
{
    if (providerKey == "chaynsapi") return kFallbackMsgChayns;
    if (providerKey == "nexosapi")  return kFallbackMsgNexos;
    if (providerKey == "retoolapi") return kFallbackMsgRetool;
    if (providerKey == "openai")    return kFallbackMsgOpenAi;
    return kFallbackMsgGeneric;
}

bool readBytes(const Json::Value& node, const char* key, size_t& out)
{
    if (!node.isObject() || !node.isMember(key)) return false;
    const auto& value = node[key];
    uint64_t parsed = 0;
    if (value.isUInt64()) {
        parsed = value.asUInt64();
    } else if (value.isInt64()) {
        const int64_t signedValue = value.asInt64();
        if (signedValue < 0) return false;
        parsed = static_cast<uint64_t>(signedValue);
    } else {
        return false;
    }
    out = static_cast<size_t>(std::min<uint64_t>(parsed, kMaximumConfiguredBytes));
    return true;
}

size_t configuredOutbound(const std::string& providerKey, const char* key, size_t fallback)
{
    const auto& customConfig = drogon::app().getCustomConfig();
    if (!customConfig.isObject() ||
        !customConfig.isMember("outbound_limits") ||
        !customConfig["outbound_limits"].isObject()) {
        return fallback;
    }

    const auto& limits = customConfig["outbound_limits"];
    size_t resolved = 0;

    if (limits.isMember(providerKey) && readBytes(limits[providerKey], key, resolved)) {
        return resolved;
    }
    if (limits.isMember("default") && readBytes(limits["default"], key, resolved)) {
        return resolved;
    }
    return fallback;
}

Json::StreamWriterBuilder& compactWriter()
{
    static thread_local Json::StreamWriterBuilder writer = [] {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return builder;
    }();
    return writer;
}

}  // namespace

namespace continuity {

size_t outboundMaxRequestBytes(const std::string& providerKey)
{
    return configuredOutbound(providerKey, "max_request_bytes",
                              builtinRequestFallback(providerKey));
}

size_t outboundMaxMessageBytes(const std::string& providerKey)
{
    return configuredOutbound(providerKey, "max_message_bytes",
                              builtinMessageFallback(providerKey));
}

std::vector<size_t> degradationLadder(size_t baseBudget, size_t steps)
{
    std::vector<size_t> ladder;
    ladder.push_back(baseBudget);
    size_t current = baseBudget;
    for (size_t index = 1; index < steps && current > 0; ++index) {
        current /= 2;
        if (current == ladder.back()) break;
        ladder.push_back(current);
    }
    if (ladder.back() != 0) ladder.push_back(0);
    return ladder;
}

size_t compactBodyBytes(const Json::Value& body)
{
    return Json::writeString(compactWriter(), body).size();
}

OutboundCheck checkOutboundSize(const std::string& providerKey, size_t bodyBytes)
{
    OutboundCheck check;
    check.actualBytes = bodyBytes;
    check.limitBytes = outboundMaxRequestBytes(providerKey);
    check.withinLimit = (check.limitBytes == 0) || (bodyBytes <= check.limitBytes);
    return check;
}

OutboundCheck checkOutboundSize(const std::string& providerKey, const Json::Value& body)
{
    return checkOutboundSize(providerKey, compactBodyBytes(body));
}

}  // namespace continuity
