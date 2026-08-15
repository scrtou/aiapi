#include <application/generation/core/AiApiUseCase.h>

#include <application/generation/contracts/IResponseSink.h>
#include <application/generation/core/GenerationService.h>
#include <application/generation/core/RequestAdapters.h>

#include <platform/Log.h>
#include <json/json.h>

#include <sstream>
#include <utility>

namespace {

aiapi::Error internalError(std::string message)
{
    aiapi::Error error;
    error.httpStatus = 500;
    error.type = "internal_error";
    error.message = std::move(message);
    return error;
}

aiapi::Error invalidRequestError(std::string message)
{
    aiapi::Error error;
    error.httpStatus = 400;
    error.type = "invalid_request_error";
    error.message = std::move(message);
    error.code = "invalid_json";
    return error;
}

aiapi::Error unavailableError(TaskSubmitResult result)
{
    aiapi::Error error;
    error.httpStatus = 503;
    error.type = "service_unavailable";
    switch (result) {
        case TaskSubmitResult::QueueFull:
            error.message = "Server is busy, please retry later";
            error.code = "queue_full";
            error.retryAfterSeconds = 1;
            break;
        case TaskSubmitResult::ShuttingDown:
        case TaskSubmitResult::Stopped:
            error.message = "Server is shutting down";
            error.code = "shutting_down";
            break;
        case TaskSubmitResult::Accepted:
            error.message = "Background task accepted";
            break;
    }
    return error;
}

aiapi::SubmissionOutcome toSubmissionOutcome(TaskSubmitResult result)
{
    switch (result) {
        case TaskSubmitResult::Accepted: return aiapi::SubmissionOutcome::Accepted;
        case TaskSubmitResult::QueueFull: return aiapi::SubmissionOutcome::QueueFull;
        case TaskSubmitResult::ShuttingDown: return aiapi::SubmissionOutcome::ShuttingDown;
        case TaskSubmitResult::Stopped: return aiapi::SubmissionOutcome::Stopped;
    }
    return aiapi::SubmissionOutcome::Stopped;
}

aiapi::Error toUseCaseError(const platform::Error& source)
{
    aiapi::Error result;
    result.httpStatus = source.httpStatus();
    result.type = source.type();
    result.message = source.message.empty() ? "Internal server error" : source.message;
    result.detail = source.detail;
    result.providerCode = source.providerCode;
    if (source.code == platform::ErrorCode::Conflict) {
        result.code = "concurrent_request";
    } else if (source.code == platform::ErrorCode::Cancelled) {
        result.code = "cancelled";
    }
    return result;
}

bool parseJsonBody(const std::string& body, Json::Value& out)
{
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(body);
    return Json::parseFromStream(builder, stream, &out, &errors);
}

void invokeCompletion(const aiapi::IAiApiUseCase::Completion& completion,
                      const aiapi::GenerationResult& result,
                      const std::shared_ptr<IResponseSink>& sink)
{
    if (!completion) {
        return;
    }
    try {
        completion(result, sink);
    } catch (const std::exception& exception) {
        LOG_ERROR << "[AI use case] transport completion threw: " << exception.what();
    } catch (...) {
        LOG_ERROR << "[AI use case] transport completion threw an unknown exception";
    }
}

void persistResponseRecord(IResponseIndex* responses,
                           const std::shared_ptr<IResponseSink>& sink)
{
    if (!responses || !sink) {
        return;
    }
    const auto* recordSink = dynamic_cast<const IResponsePersistenceSink*>(sink.get());
    if (!recordSink) {
        return;
    }
    const auto record = recordSink->responseRecord();
    if (!record.has_value()) {
        return;
    }
    responses->storeResponse(record->responseId, record->serializedJson);
}

}  // namespace

AiApiUseCase::AiApiUseCase(IProviderRegistry* providers,
                           chatSession* sessions,
                           IResponseIndex* responses,
                           session::IExecutionGate* executionGate,
                           IChannelCatalog* channels,
                           IBackgroundExecutor* executor,
                           Json::Value runtimeConfig)
    : providers_(providers),
      sessions_(sessions),
      responses_(responses),
      executionGate_(executionGate),
      channels_(channels),
      executor_(executor),
      runtimeConfig_(std::move(runtimeConfig))
{
}

aiapi::SubmissionResult AiApiUseCase::submitGeneration(
    aiapi::GenerationInput input,
    SinkFactory makeSink,
    Completion onComplete)
{
    Json::Value requestBody;
    if (!parseJsonBody(input.jsonBody, requestBody) || !requestBody.isObject()) {
        aiapi::SubmissionResult result;
        result.outcome = aiapi::SubmissionOutcome::InvalidRequest;
        result.error = invalidRequestError("Invalid JSON in request body");
        return result;
    }

    GenerationRequest request = input.endpoint == aiapi::Endpoint::Responses
        ? RequestAdapters::buildGenerationRequestFromResponses(requestBody, input.headers)
        : RequestAdapters::buildGenerationRequestFromChat(requestBody, input.headers);
    request.provider = input.provider;

    aiapi::GenerationPresentation presentation;
    presentation.model = request.model;
    presentation.nativeResponsesToolItems =
        request.clientInfo.isObject() &&
        request.clientInfo.get("client_type", "").asString() == "Codex";
    presentation.inputTokensEstimated =
        static_cast<int>(request.currentInput.length() / 4);

    // Capture all borrowed collaborators at admission.  AppWiring may revoke
    // the controller binding during rollback/shutdown, but a queued job must
    // keep using the context-owned collaborators it was admitted with.
    auto* const providers = providers_;
    auto* const sessions = sessions_;
    auto* const responses = responses_;
    auto* const executionGate = executionGate_;
    auto* const channels = channels_;
    auto* const executor = executor_;
    const Json::Value runtimeConfig = runtimeConfig_;

    if (!providers || !sessions || !responses || !executionGate || !executor) {
        aiapi::SubmissionResult result;
        result.outcome = aiapi::SubmissionOutcome::Stopped;
        result.error = unavailableError(TaskSubmitResult::Stopped);
        return result;
    }

    const auto submitted = executor->submit(
        input.endpoint == aiapi::Endpoint::Responses
            ? "responses_generation"
            : "chat_generation",
        [request = std::move(request), presentation = std::move(presentation),
         makeSink = std::move(makeSink), onComplete = std::move(onComplete),
         providers, sessions, responses, executionGate, channels,
         runtimeConfig = std::move(runtimeConfig)]() mutable {
            std::shared_ptr<IResponseSink> sink;
            try {
                sink = makeSink ? makeSink(presentation) : nullptr;
            } catch (const std::exception& exception) {
                LOG_ERROR << "[AI use case] response sink construction threw: "
                          << exception.what();
                aiapi::GenerationResult result;
                result.error = internalError("Failed to initialize response sink");
                invokeCompletion(onComplete, result, sink);
                return;
            } catch (...) {
                LOG_ERROR << "[AI use case] response sink construction threw an unknown exception";
                aiapi::GenerationResult result;
                result.error = internalError("Failed to initialize response sink");
                invokeCompletion(onComplete, result, sink);
                return;
            }

            if (!sink) {
                aiapi::GenerationResult result;
                result.error = internalError("Response sink is unavailable");
                invokeCompletion(onComplete, result, sink);
                return;
            }

            GenerationService generation(
                providers, sessions, responses, executionGate, channels, runtimeConfig);
            const auto executionError = generation.runGuarded(
                request, *sink, session::ConcurrencyPolicy::RejectConcurrent);

            aiapi::GenerationResult result;
            if (executionError.has_value()) {
                result.error = toUseCaseError(*executionError);
            } else if (request.endpointType == EndpointType::Responses) {
                // The sink owns OpenAI JSON/SSE encoding; this facade owns the
                // response-index write so Controllers never touch persistence.
                persistResponseRecord(responses, sink);
            }
            invokeCompletion(onComplete, result, sink);
        });

    aiapi::SubmissionResult result;
    result.outcome = toSubmissionOutcome(submitted);
    if (submitted != TaskSubmitResult::Accepted) {
        result.error = unavailableError(submitted);
    }
    return result;
}

aiapi::ModelCatalogResult AiApiUseCase::modelCatalog(const std::string& provider) const
{
    aiapi::ModelCatalogResult result;
    if (!providers_) {
        return result;
    }
    if (const auto catalog = providers_->findModelCatalog(provider)) {
        result.outcome = aiapi::ModelCatalogOutcome::Found;
        result.catalog = catalog->getModels();
        return result;
    }

    return result;
}

aiapi::StoredResponseResult AiApiUseCase::getResponse(const std::string& responseId)
{
    aiapi::StoredResponseResult result;
    if (!responses_ || !responses_->tryGetResponse(responseId, result.jsonBody)) {
        return result;
    }

    Json::Value parsed;
    if (!parseJsonBody(result.jsonBody, parsed)) {
        result.outcome = aiapi::StoredResponseOutcome::Corrupt;
        result.jsonBody.clear();
        return result;
    }
    result.outcome = aiapi::StoredResponseOutcome::Found;
    return result;
}

aiapi::DeleteResponseResult AiApiUseCase::deleteResponse(const std::string& responseId)
{
    aiapi::DeleteResponseResult result;
    if (responses_ && responses_->erase(responseId)) {
        result.outcome = aiapi::DeleteResponseOutcome::Deleted;
    }
    return result;
}
