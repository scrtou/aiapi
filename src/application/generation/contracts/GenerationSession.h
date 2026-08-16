#ifndef GENERATION_SESSION_H
#define GENERATION_SESSION_H

#include <domain/model/BridgeWireFormat.h>
#include <domain/model/ImageInfo.h>
#include <domain/model/SessionData.h>
#include <application/generation/contracts/CurrentTurnKind.h>
#include <json/json.h>

#include <cstddef>
#include <string>
#include <vector>

// Execution aggregate used by the generation pipeline. JSON fields are owned
// by the Session persistence/provider boundary and do not belong in domain models.
struct session_st
{
    struct RequestData
    {
        std::string api;
        std::string model;
        std::string systemPrompt;
        std::string message;
        std::vector<ImageInfo> images;
        // Request-scoped semantic marker. It is copied from the normalized
        // request before persisted session state is merged and is not part of
        // the durable session snapshot.
        CurrentTurnKind currentTurnKind = CurrentTurnKind::Durable;
        Json::Value tools;
        Json::Value toolDefinitionsSource;
        // Request-scoped diagnostic value.  It records the callable tool
        // definitions supplied by the current client request before session
        // continuation can restore definitions from an earlier turn.
        std::size_t clientRequestedToolCount = 0;
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
        // Correlation ledger for client tool-result delivery.  It belongs to
        // the stable upstream conversation rather than to a wire protocol:
        // emitted calls await a result in pendingToolCallIds; accepted results
        // remain in consumedToolResultIds to make full-transcript retries
        // idempotent.
        std::vector<std::string> pendingToolCallIds;
        std::vector<std::string> consumedToolResultIds;
        // The latest client-declared append-only user-text snapshot.  It is
        // deliberately protocol-neutral: adapters opt in per input fragment
        // when their upstream wire format repeats historical text.
        std::string replayableInputTextSnapshot;
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

#endif  // GENERATION_SESSION_H
