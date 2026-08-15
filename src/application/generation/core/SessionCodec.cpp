#include <application/generation/core/SessionCodec.h>

#include <application/generation/tooling/BridgeProtocolCodec.h>

namespace sessioncodec {
namespace {

// ---- 取值辅助：缺键一律回落默认值，保证老快照可读 ----
std::string getStr(const Json::Value& v, const char* key, const std::string& def = "")
{
    if (!v.isObject() || !v.isMember(key) || !v[key].isString()) return def;
    return v[key].asString();
}

bool getBool(const Json::Value& v, const char* key, bool def)
{
    if (!v.isObject() || !v.isMember(key) || !v[key].isBool()) return def;
    return v[key].asBool();
}

int getInt(const Json::Value& v, const char* key, int def)
{
    if (!v.isObject() || !v.isMember(key) || !v[key].isIntegral()) return def;
    return v[key].asInt();
}

int64_t getI64(const Json::Value& v, const char* key, int64_t def)
{
    if (!v.isObject() || !v.isMember(key) || !v[key].isIntegral()) return def;
    return v[key].asInt64();
}

// Json::Value 原样透传：缺键返回 null，调用方按字段语义决定是否补默认数组。
Json::Value getJson(const Json::Value& v, const char* key)
{
    if (!v.isObject() || !v.isMember(key)) return Json::Value();
    return v[key];
}

// ---- 枚举稳定映射：显式整数，禁止依赖声明顺序 ----
int apiTypeToInt(ApiType t)
{
    return t == ApiType::Responses ? 1 : 0;
}

ApiType apiTypeFromInt(int v)
{
    return v == 1 ? ApiType::Responses : ApiType::ChatCompletions;
}

int bridgeFormatToInt(toolcall::BridgeWireFormat f)
{
    switch (f) {
        case toolcall::BridgeWireFormat::Json: return 1;
        case toolcall::BridgeWireFormat::Xml:  return 2;
        default:                               return 0; // Unset
    }
}

toolcall::BridgeWireFormat bridgeFormatFromInt(int v)
{
    switch (v) {
        case 1:  return toolcall::BridgeWireFormat::Json;
        case 2:  return toolcall::BridgeWireFormat::Xml;
        default: return toolcall::BridgeWireFormat::Unset;
    }
}

} // namespace

Json::Value encodeSession(const session_st& session)
{
    Json::Value root(Json::objectValue);
    // schema 版本：后续字段演进时用于判定兼容策略。
    root["v"] = 1;

    // ---- request ----
    Json::Value req(Json::objectValue);
    req["api"]               = session.request.api;
    req["model"]             = session.request.model;
    req["systemPrompt"]      = session.request.systemPrompt;
    req["message"]           = session.request.message;
    req["toolChoice"]        = session.request.toolChoice;
    req["parallelToolCalls"] = session.request.parallelToolCalls;
    req["rawMessage"]        = session.request.rawMessage;
    req["tools"]             = session.request.tools;
    req["toolsRaw"]          = session.request.toolsRaw;

    Json::Value images(Json::arrayValue);
    for (const auto& img : session.request.images) {
        Json::Value one(Json::objectValue);
        one["base64Data"]  = img.base64Data;
        one["mediaType"]   = img.mediaType;
        one["uploadedUrl"] = img.uploadedUrl;
        one["width"]       = img.width;
        one["height"]      = img.height;
        images.append(one);
    }
    req["images"] = images;
    root["request"] = req;

    // ---- response ----
    Json::Value resp(Json::objectValue);
    resp["message"]        = session.response.message;
    resp["apiData"]        = session.response.apiData;
    resp["responseId"]     = session.response.responseId;
    resp["lastResponseId"] = session.response.lastResponseId;
    root["response"] = resp;

    // ---- state ----
    Json::Value st(Json::objectValue);
    st["apiType"]               = apiTypeToInt(session.state.apiType);
    st["hasPreviousResponseId"] = session.state.hasPreviousResponseId;
    st["isContinuation"]        = session.state.isContinuation;
    st["conversationId"]        = session.state.conversationId;
    st["nextSessionId"]         = session.state.nextSessionId;
    st["createdAt"]             = static_cast<Json::Int64>(session.state.createdAt);
    st["lastActiveAt"]          = static_cast<Json::Int64>(session.state.lastActiveAt);
    st["requestId"]             = session.state.requestId;
    st["contextConversationId"] = session.state.contextConversationId;
    st["contextLength"]         = session.state.contextLength;
    st["contextIsFull"]         = session.state.contextIsFull;
    root["state"] = st;

    // ---- provider ----
    // prevProviderKey 是回连 chayns 上游会话线程的关键，丢失即导致「续聊落到新线程」。
    Json::Value pv(Json::objectValue);
    pv["prevProviderKey"]              = session.provider.prevProviderKey;
    pv["toolBridgeTrigger"]            = session.provider.toolBridgeTrigger;
    pv["toolBridgeFormat"]             = bridgeFormatToInt(session.provider.toolBridgeFormat);
    pv["toolBridgeAllowFormatFallback"] = session.provider.toolBridgeAllowFormatFallback;
    pv["supportsToolCalls"]            = session.provider.supportsToolCalls;
    pv["clientInfo"]                   = session.provider.clientInfo;
    pv["messageContext"]               = session.provider.messageContext;
    root["provider"] = pv;

    return root;
}

session_st decodeSession(const Json::Value& payload)
{
    session_st s; // 全部字段先取结构体默认值
    if (!payload.isObject()) return s;

    const Json::Value req = getJson(payload, "request");
    s.request.api               = getStr(req, "api");
    s.request.model             = getStr(req, "model");
    s.request.systemPrompt      = getStr(req, "systemPrompt");
    s.request.message           = getStr(req, "message");
    s.request.toolChoice        = getStr(req, "toolChoice");
    s.request.parallelToolCalls = getBool(req, "parallelToolCalls", true);
    s.request.rawMessage        = getStr(req, "rawMessage");
    s.request.tools             = getJson(req, "tools");
    s.request.toolsRaw          = getJson(req, "toolsRaw");

    const Json::Value images = getJson(req, "images");
    if (images.isArray()) {
        for (const auto& one : images) {
            ImageInfo img;
            img.base64Data  = getStr(one, "base64Data");
            img.mediaType   = getStr(one, "mediaType");
            img.uploadedUrl = getStr(one, "uploadedUrl");
            img.width       = getInt(one, "width", 0);
            img.height      = getInt(one, "height", 0);
            s.request.images.push_back(img);
        }
    }

    const Json::Value resp = getJson(payload, "response");
    s.response.message        = getJson(resp, "message");
    s.response.apiData        = getJson(resp, "apiData");
    s.response.responseId     = getStr(resp, "responseId");
    s.response.lastResponseId = getStr(resp, "lastResponseId");

    const Json::Value st = getJson(payload, "state");
    s.state.apiType               = apiTypeFromInt(getInt(st, "apiType", 0));
    s.state.hasPreviousResponseId = getBool(st, "hasPreviousResponseId", false);
    s.state.isContinuation        = getBool(st, "isContinuation", false);
    s.state.conversationId        = getStr(st, "conversationId");
    s.state.nextSessionId         = getStr(st, "nextSessionId");
    s.state.createdAt             = static_cast<time_t>(getI64(st, "createdAt", 0));
    s.state.lastActiveAt          = static_cast<time_t>(getI64(st, "lastActiveAt", 0));
    s.state.requestId             = getStr(st, "requestId");
    s.state.contextConversationId = getStr(st, "contextConversationId");
    s.state.contextLength         = getInt(st, "contextLength", 0);
    s.state.contextIsFull         = getBool(st, "contextIsFull", false);

    const Json::Value pv = getJson(payload, "provider");
    s.provider.prevProviderKey    = getStr(pv, "prevProviderKey");
    s.provider.toolBridgeTrigger  = getStr(pv, "toolBridgeTrigger");
    s.provider.toolBridgeFormat   = bridgeFormatFromInt(getInt(pv, "toolBridgeFormat", 0));
    s.provider.toolBridgeAllowFormatFallback =
        getBool(pv, "toolBridgeAllowFormatFallback", false);
    s.provider.supportsToolCalls  = getBool(pv, "supportsToolCalls", true);
    s.provider.clientInfo         = getJson(pv, "clientInfo");

    // messageContext 语义上必须是数组：老快照缺失时保持空数组而非 null，
    // 否则 addMessageToContext 的 append 行为会因类型不符而异常。
    Json::Value mc = getJson(pv, "messageContext");
    s.provider.messageContext = mc.isArray() ? mc : Json::Value(Json::arrayValue);

    return s;
}

} // namespace sessioncodec
