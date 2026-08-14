#include <sessionManager/core/GenerationService.h>
#include <sessionManager/core/GenerationPipeline.h>

#include <memory>

GenerationService::GenerationService(
    IProviderRegistry* providerRegistry,
    chatSession* sessionStore,
    IResponseIndex* responseIndex,
    session::IExecutionGate* executionGate,
    IChannelCatalog* channelCatalog)
    : pipeline_(std::make_unique<generation::GenerationPipeline>(
          providerRegistry, sessionStore, responseIndex, executionGate, channelCatalog))
{
}

GenerationService::~GenerationService() = default;

std::optional<error::AppError> GenerationService::runGuarded(
    const GenerationRequest& request,
    IResponseSink& sink,
    session::ConcurrencyPolicy policy)
{
    return pipeline_->run(request, sink, policy);
}
