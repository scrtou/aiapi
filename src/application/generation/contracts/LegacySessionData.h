#ifndef LEGACY_SESSION_DATA_H
#define LEGACY_SESSION_DATA_H

#include <domain/model/BridgeWireFormat.h>
#include <domain/model/ImageInfo.h>
#include <domain/model/SessionData.h>
#include <json/json.h>

#include <string>
#include <vector>

// Transitional aggregate used by the pre-P7 generation pipeline.  JSON fields
// are explicitly edge-owned here and must not move back into domain models.
struct session_st
{
    struct RequestData
    {
        std::string api;
        std::string model;
        std::string systemPrompt;
        std::string message;
        std::vector<ImageInfo> images;
        Json::Value tools;
        Json::Value toolsRaw;
        std::string toolChoice;
        bool parallelToolCalls = true;
        std::string rawMessage;
    };

    struct ResponseData
    {
        Json::Value message;
        Json::Value apiData;
        std::string responseId;
        std::string lastResponseId;
    };

    struct ProviderContext
    {
        std::string prevProviderKey;
        // Request-scoped restart recovery hints. They deliberately are not
        // serialized: the durable snapshot's prevProviderKey/request.model
        // are copied here only while a continuation is being materialized.
        std::string prevProviderFallbackKey;
        std::string prevProviderFallbackModel;
        std::string toolBridgeTrigger;
        toolcall::BridgeWireFormat toolBridgeFormat = toolcall::BridgeWireFormat::Unset;
        bool toolBridgeAllowFormatFallback = false;
        bool supportsToolCalls = true;
        Json::Value clientInfo;
        Json::Value messageContext = Json::Value(Json::arrayValue);
    };

    RequestData request;
    ResponseData response;
    SessionState state;
    ProviderContext provider;

    void clearMessageContext() { provider.messageContext.clear(); }
    void addMessageToContext(const Json::Value& message) { provider.messageContext.append(message); }
    bool isResponseApi() const { return state.apiType == ApiType::Responses; }
    bool isChatApi() const { return state.apiType == ApiType::ChatCompletions; }
};

#endif  // LEGACY_SESSION_DATA_H
