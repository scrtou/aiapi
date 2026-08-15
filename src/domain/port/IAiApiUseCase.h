#pragma once

#include <domain/model/AiApiData.h>

#include <functional>
#include <memory>
#include <string>

#include <json/json.h>

class IResponseSink;

namespace aiapi {

/**
 * The only runtime collaborator published to AiApiController.
 *
 * The controller owns HTTP validation and IO callbacks. This use case owns
 * request normalization, queue admission, generation execution, provider
 * catalog lookup, and Responses persistence/read/delete workflows.
 */
class IAiApiUseCase
{
  public:
    using Completion = std::function<void(
        const GenerationResult&, const std::shared_ptr<IResponseSink>&)>;

    /**
     * Transport callbacks passed to the protocol-owned Sink factory.
     * Controllers provide only IO bindings; they do not construct a concrete
     * Chat/Responses sink.
     */
    struct ResponseBinding {
        bool stream = false;
        std::function<void(const Json::Value&, int)> jsonResponse;
        std::function<bool(const std::string&)> streamWriter;
        std::function<void()> close;
    };

    virtual ~IAiApiUseCase() = default;

    virtual SubmissionResult submitGeneration(
        GenerationInput input, ResponseBinding binding, Completion onComplete) = 0;

    virtual ModelCatalogResult modelCatalog(const std::string& provider) const = 0;
    virtual StoredResponseResult getResponse(const std::string& responseId) = 0;
    virtual DeleteResponseResult deleteResponse(const std::string& responseId) = 0;
};

}  // namespace aiapi
