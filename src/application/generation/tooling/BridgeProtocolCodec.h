#ifndef AIAPI_BRIDGE_PROTOCOL_CODEC_H
#define AIAPI_BRIDGE_PROTOCOL_CODEC_H

#include <application/generation/actionProtocol/ActionProtocolCompiler.h>
#include <application/generation/contracts/GenerationEvent.h>

#include <domain/model/BridgeWireFormat.h>
#include <json/json.h>
#include <memory>
#include <string>
#include <vector>

namespace toolcall {

// BridgeWireFormat 已迁至 domain/model/BridgeWireFormat.h

const char* bridgeWireFormatName(BridgeWireFormat format);
BridgeWireFormat parseBridgeWireFormat(const std::string& value,
                                       BridgeWireFormat fallback);

// Resolve once for each request. Override order is:
// global -> channel -> client -> model.
BridgeWireFormat resolveBridgeWireFormat(const Json::Value& toolBridgeConfig,
                                         const std::string& clientType,
                                         const std::string& channel,
                                         const std::string& model);
bool resolveBridgeFormatFallback(const Json::Value& toolBridgeConfig);

struct BridgeDefinitionOptions {
    bool includeDescriptions = false;
    int maxDescriptionChars = 160;
    bool fullSchema = false;
};

struct BridgePolicyOptions {
    std::string clientType;
    std::string channel;
    std::string model;
    std::string sentinel;
    std::string toolChoice = "auto";
    std::string forcedToolName;
    bool parallelToolCalls = false;
    bool requireToolForCurrentRequest = false;
};

struct BridgeDecodeResult {
    bool matched = false;
    bool valid = false;
    std::string protocol;
    std::string text;
    std::vector<generation::ToolCallDone> toolCalls;
    actionproto::CompileDiagnostic diagnostic;
};

class IBridgeProtocolCodec {
public:
    virtual ~IBridgeProtocolCodec() = default;

    virtual BridgeWireFormat format() const = 0;
    virtual std::string encodeToolDefinitions(
        const Json::Value& tools,
        const BridgeDefinitionOptions& options) const = 0;
    virtual std::string buildPolicy(const BridgePolicyOptions& options) const = 0;
    virtual std::string encodeToolCallsForHistory(
        const Json::Value& toolCalls,
        const BridgePolicyOptions& options) const = 0;
    virtual std::string encodeToolResultForHistory(
        const std::string& toolCallId,
        const std::string& content) const = 0;
    virtual void transformHistory(Json::Value& messageContext,
                                  const BridgePolicyOptions& options) const = 0;
    virtual BridgeDecodeResult decodeResponse(
        const std::string& input,
        const BridgePolicyOptions& options) const = 0;
    virtual std::string buildRetryPrompt(const BridgePolicyOptions& options) const = 0;
};

std::shared_ptr<IBridgeProtocolCodec> createBridgeProtocolCodec(
    BridgeWireFormat format);

}  // namespace toolcall

#endif
