#ifndef AIAPI_GENERATION_PIPELINE_H
#define AIAPI_GENERATION_PIPELINE_H

#include <application/generation/contracts/GenerationRequest.h>
#include <application/generation/contracts/IResponseSink.h>
#include <application/generation/core/GenerationResponsePipeline.h>
#include <domain/port/IProviderRegistry.h>
#include <platform/Cancellation.h>
#include <platform/Deadline.h>
#include <platform/result/Error.h>
#include <application/generation/core/SessionExecutionGate.h>

#include <json/json.h>
#include <optional>

class chatSession;
class IProviderRegistry;
class IResponseIndex;
class IChannelCatalog;

namespace generation {

// P7-W1 application pipeline.  It owns the request-scoped orchestration while
// pure tool/request rules remain in the tooling namespace.
class GenerationPipeline {
public:
    GenerationPipeline(IProviderRegistry* providerRegistry,
                       chatSession* sessionStore,
                       IResponseIndex* responseIndex,
                       session::IExecutionGate* executionGate,
                       IChannelCatalog* channelCatalog,
                       Json::Value runtimeConfig = Json::Value(Json::objectValue));

    std::optional<platform::Error> run(
        const GenerationRequest& request,
        IResponseSink& sink,
        session::ConcurrencyPolicy policy);

private:
    struct ToolBridgeState {
        bool supportsToolCalls = true;
        bool toolChoiceNone = false;
        bool hasToolDefinitions = false;
    };

    // A strict bridge retry is a correction sent after the provider has
    // already produced one response.  It must therefore be delivered as a
    // follow-up on the upstream thread created by that first invocation,
    // even when the local request itself started a new conversation.
    enum class ProviderInvocationMode {
        Default,
        BridgeCorrectionFollowUp,
    };

    std::optional<platform::Error> execute(
        session_st& session,
        IResponseSink& sink,
        bool stream,
        session::ConcurrencyPolicy policy);

    static session_st materializeRequest(const GenerationRequest& request);
    static std::string executionKey(const session_st& session);
    ToolBridgeState prepareToolBridge(session_st& session) const;
    void retryStrictToolBridgeResponse(
        session_st& session,
        const ToolBridgeState& bridge,
        const platform::CancellationToken& cancellation,
        platform::Deadline deadline);

    std::optional<platform::Error> invokeProvider(
        session_st& session,
        const platform::CancellationToken& cancellation,
        platform::Deadline deadline,
        ProviderInvocationMode mode = ProviderInvocationMode::Default);

    static provider::ProviderRequest providerRequestFromSession(
        const session_st& session,
        ProviderInvocationMode mode = ProviderInvocationMode::Default);
    static void applyProviderResponse(
        session_st& session,
        const provider::ProviderResponse& response);

    IProviderRegistry* providerRegistry_ = nullptr;
    chatSession* sessionStore_ = nullptr;
    IResponseIndex* responseIndex_ = nullptr;
    session::IExecutionGate* executionGate_ = nullptr;
    Json::Value runtimeConfig_;
    GenerationResponsePipeline responsePipeline_;
};

} // namespace generation

#endif
