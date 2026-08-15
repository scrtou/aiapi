#ifndef GENERATION_SERVICE_H
#define GENERATION_SERVICE_H

#include <application/generation/contracts/GenerationRequest.h>
#include <application/generation/contracts/IResponseSink.h>
#include <application/generation/core/SessionExecutionGate.h>
#include <platform/result/Error.h>

#include <json/json.h>
#include <memory>
#include <optional>

class chatSession;
class IProviderRegistry;
class IResponseIndex;
class IChannelCatalog;

namespace generation {
class GenerationPipeline;
}

/**
 * Public application port kept stable for controllers and AiApiUseCase.
 * P7-W1 moves orchestration into GenerationPipeline; this class deliberately
 * contains no generation rules or provider/session fallback paths.
 */
class GenerationService {
public:
    GenerationService(IProviderRegistry* providerRegistry,
                      chatSession* sessionStore,
                      IResponseIndex* responseIndex,
                      session::IExecutionGate* executionGate,
                      IChannelCatalog* channelCatalog = nullptr,
                      Json::Value runtimeConfig = Json::Value(Json::objectValue));
    ~GenerationService();

    GenerationService(const GenerationService&) = delete;
    GenerationService& operator=(const GenerationService&) = delete;
    GenerationService(GenerationService&&) = delete;
    GenerationService& operator=(GenerationService&&) = delete;

    std::optional<platform::Error> runGuarded(
        const GenerationRequest& request,
        IResponseSink& sink,
        session::ConcurrencyPolicy policy = session::ConcurrencyPolicy::RejectConcurrent);

private:
    std::unique_ptr<generation::GenerationPipeline> pipeline_;
};

#endif
