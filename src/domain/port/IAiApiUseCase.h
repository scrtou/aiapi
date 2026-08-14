#pragma once

#include <domain/model/AiApiData.h>

#include <functional>
#include <memory>
#include <string>

class IResponseSink;

namespace aiapi {

/**
 * The only runtime collaborator published to AiApiController.
 *
 * The controller owns HTTP validation and protocol sinks.  This use case owns
 * request normalization, queue admission, generation execution, provider
 * catalog lookup, and Responses persistence/read/delete workflows.
 */
class IAiApiUseCase
{
  public:
    using SinkFactory = std::function<std::shared_ptr<IResponseSink>(
        const GenerationPresentation&)>;
    using Completion = std::function<void(
        const GenerationResult&, const std::shared_ptr<IResponseSink>&)>;

    virtual ~IAiApiUseCase() = default;

    virtual SubmissionResult submitGeneration(
        GenerationInput input, SinkFactory makeSink, Completion onComplete) = 0;

    virtual ModelCatalogResult modelCatalog(const std::string& provider) const = 0;
    virtual StoredResponseResult getResponse(const std::string& responseId) = 0;
    virtual DeleteResponseResult deleteResponse(const std::string& responseId) = 0;
};

}  // namespace aiapi
