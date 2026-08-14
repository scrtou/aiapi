#ifndef AIAPI_GENERATION_PIPELINE_H
#define AIAPI_GENERATION_PIPELINE_H

#include <sessionManager/contracts/GenerationRequest.h>
#include <sessionManager/contracts/IResponseSink.h>
#include <sessionManager/core/Errors.h>
#include <sessionManager/core/GenerationResponsePipeline.h>
#include <domain/port/IProviderRegistry.h>
#include <platform/Cancellation.h>
#include <platform/Deadline.h>
#include <sessionManager/core/SessionExecutionGate.h>

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
                       IChannelCatalog* channelCatalog);

    std::optional<error::AppError> run(
        const GenerationRequest& request,
        IResponseSink& sink,
        session::ConcurrencyPolicy policy);

private:
    struct ToolBridgeState {
        bool supportsToolCalls = true;
        bool toolChoiceNone = false;
        bool hasToolDefinitions = false;
    };

    std::optional<error::AppError> execute(
        session_st& session,
        IResponseSink& sink,
        bool stream,
        session::ConcurrencyPolicy policy);

    static session_st materializeRequest(const GenerationRequest& request);
    static std::string executionKey(const session_st& session);
    ToolBridgeState prepareToolBridge(session_st& session) const;
    void retryCodexBridgeResponse(
        session_st& session,
        const ToolBridgeState& bridge,
        const platform::CancellationToken& cancellation,
        platform::Deadline deadline);

    std::optional<platform::Error> invokeProvider(
        session_st& session,
        const platform::CancellationToken& cancellation,
        platform::Deadline deadline);

    static provider::ProviderRequest providerRequestFromSession(
        const session_st& session);
    static void applyProviderResponse(
        session_st& session,
        const provider::ProviderResponse& response);

    IProviderRegistry* providerRegistry_ = nullptr;
    chatSession* sessionStore_ = nullptr;
    IResponseIndex* responseIndex_ = nullptr;
    session::IExecutionGate* executionGate_ = nullptr;
    GenerationResponsePipeline responsePipeline_;
};

} // namespace generation

#endif
