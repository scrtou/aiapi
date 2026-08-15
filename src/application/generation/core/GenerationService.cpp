#include <application/generation/core/GenerationService.h>
#include <application/generation/core/GenerationPipeline.h>

#include <memory>

GenerationService::GenerationService(
    IProviderRegistry* providerRegistry,
    chatSession* sessionStore,
    IResponseIndex* responseIndex,
    session::IExecutionGate* executionGate,
    IChannelCatalog* channelCatalog,
    Json::Value runtimeConfig)
    : pipeline_(std::make_unique<generation::GenerationPipeline>(
          providerRegistry, sessionStore, responseIndex, executionGate, channelCatalog,
          std::move(runtimeConfig)))
{
}

GenerationService::~GenerationService() = default;

std::optional<platform::Error> GenerationService::runGuarded(
    const GenerationRequest& request,
    IResponseSink& sink,
    session::ConcurrencyPolicy policy)
{
    return pipeline_->run(request, sink, policy);
}
