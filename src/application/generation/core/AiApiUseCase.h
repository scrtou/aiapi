#pragma once

#include <domain/port/IAiApiUseCase.h>
#include <domain/port/IBackgroundExecutor.h>
#include <domain/port/IChannelCatalog.h>
#include <domain/port/IExecutionGate.h>
#include <domain/port/IProviderRegistry.h>
#include <domain/port/IResponseIndex.h>
#include <json/json.h>

class chatSession;

/**
 * Legacy-generation adapter behind the controller-facing AI use-case port.
 *
 * It is intentionally kept beside GenerationService until P7 replaces that
 * pipeline.  The controller sees only IAiApiUseCase; this class is the single
 * composition seam allowed to coordinate the legacy collaborators.
 */
class AiApiUseCase final : public aiapi::IAiApiUseCase
{
  public:
    AiApiUseCase(IProviderRegistry* providers,
                 chatSession* sessions,
                 IResponseIndex* responses,
                 session::IExecutionGate* executionGate,
                 IChannelCatalog* channels,
                 IBackgroundExecutor* executor,
                 Json::Value runtimeConfig = Json::Value(Json::objectValue));

    aiapi::SubmissionResult submitGeneration(
        aiapi::GenerationInput input,
        SinkFactory makeSink,
        Completion onComplete) override;

    aiapi::ModelCatalogResult modelCatalog(const std::string& provider) const override;
    aiapi::StoredResponseResult getResponse(const std::string& responseId) override;
    aiapi::DeleteResponseResult deleteResponse(const std::string& responseId) override;

  private:
    IProviderRegistry* providers_ = nullptr;
    chatSession* sessions_ = nullptr;
    IResponseIndex* responses_ = nullptr;
    session::IExecutionGate* executionGate_ = nullptr;
    IChannelCatalog* channels_ = nullptr;
    IBackgroundExecutor* executor_ = nullptr;
    Json::Value runtimeConfig_;
};
