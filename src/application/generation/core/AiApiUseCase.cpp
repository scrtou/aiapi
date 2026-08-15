#include <application/generation/core/AiApiUseCase.h>

#include <application/generation/contracts/IResponseSink.h>
#include <application/generation/core/GenerationService.h>
#include <platform/Log.h>
#include <json/json.h>

#include <sstream>
#include <stdexcept>
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

aiapi::Error unsupportedCapabilityError(const std::string& capability)
{
    aiapi::Error error;
    error.httpStatus = 400;
    error.type = "unsupported_capability";
    error.code = "unsupported_" + capability;
    error.message = "Requested capability is not available: " + capability;
    return error;
}

struct ModelCapabilityResolution {
    GenerationCapabilities capabilities = GenerationCapabilities::all();
    bool declared = false;
};

ModelCapabilityResolution resolveModelCapabilities(IProviderRegistry* providers,
                                                    const std::string& providerId,
                                                    const std::string& modelId)
{
    ModelCapabilityResolution result;
    if (!providers || providerId.empty() || modelId.empty()) return result;

    const auto catalogProvider = providers->findModelCatalog(providerId);
    if (!catalogProvider) return result;

    try {
        const auto declared = catalogProvider->findModelCapabilities(modelId);
        if (!declared.has_value()) return result;
        result.capabilities.images = declared->images;
        result.capabilities.reasoning = declared->thinking;
        // Non-native tool models remain usable through the application Tool
        // Bridge, so functionCalling is not a hard tools gate here.
        result.declared = true;
    } catch (const std::exception& error) {
        LOG_WARN << "[AI use case] model capability lookup failed: " << error.what();
    } catch (...) {
        LOG_WARN << "[AI use case] model capability lookup failed";
    }
    return result;
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
                           Json::Value runtimeConfig,
                           std::shared_ptr<generation::protocol::ProtocolRegistry> protocolRegistry)
    : providers_(providers),
      sessions_(sessions),
      responses_(responses),
      executionGate_(executionGate),
      channels_(channels),
      executor_(executor),
      runtimeConfig_(std::move(runtimeConfig)),
      protocolRegistry_(protocolRegistry ? std::move(protocolRegistry)
                                         : generation::protocol::makeDefaultProtocolRegistry())
{
}

aiapi::SubmissionResult AiApiUseCase::submitGeneration(
    aiapi::GenerationInput input,
    ResponseBinding binding,
    Completion onComplete)
{
    Json::Value requestBody;
    if (!parseJsonBody(input.jsonBody, requestBody) || !requestBody.isObject()) {
        aiapi::SubmissionResult result;
        result.outcome = aiapi::SubmissionOutcome::InvalidRequest;
        result.error = invalidRequestError("Invalid JSON in request body");
        return result;
    }

    if (!protocolRegistry_) {
        aiapi::SubmissionResult result;
        result.outcome = aiapi::SubmissionOutcome::Stopped;
        result.error = internalError("Protocol registry is unavailable");
        return result;
    }

    GenerationRequest request;
    const generation::protocol::IProtocolResponseSinkFactory* responseSinkFactory = nullptr;
    std::string protocolOperation;
    GenerationCapabilities protocolCapabilities = GenerationCapabilities::all();
    const auto dispatch = protocolRegistry_->dispatch(
        generation::protocol::RawProtocolRequest{
            input.method, input.path, requestBody, input.headers});
    if (!dispatch.succeeded()) {
        aiapi::SubmissionResult result;
        result.outcome = aiapi::SubmissionOutcome::InvalidRequest;
        result.error = dispatch.adaptation.error.has_value()
            ? toUseCaseError(*dispatch.adaptation.error)
            : invalidRequestError("Protocol request adaptation failed");
        return result;
    }
    responseSinkFactory = dispatch.responseSinkFactory;
    protocolOperation = dispatch.operation;
    protocolCapabilities = dispatch.protocolCapabilities;
    request = *dispatch.adaptation.request;
    request.provider = input.provider;
    request.protocolCapabilities = protocolCapabilities;

    const auto modelCapabilityResolution = resolveModelCapabilities(
        providers_, request.provider, request.model);
    request.modelCapabilities = intersectCapabilities(
        request.modelCapabilities, modelCapabilityResolution.capabilities,
        GenerationCapabilities::all());
    request.modelCapabilitiesDeclared = request.modelCapabilitiesDeclared ||
        modelCapabilityResolution.declared;

    GenerationCapabilities providerCapabilities = GenerationCapabilities::all();
    if (providers_) {
        const auto provider = providers_->findChatProvider(request.provider);
        if (provider) {
            const auto capabilities = provider->capabilities();
            providerCapabilities.images = capabilities.supportsImages;
            providerCapabilities.continuity = capabilities.upstreamHistory;
            // Tool calls can be bridged by the application even when the
            // upstream has no native tool protocol. Parallel native calls
            // remain constrained by the provider capability.
            providerCapabilities.tools = true;
            providerCapabilities.parallelTools = capabilities.nativeToolCalls;
        }
    }
    request.providerCapabilities = providerCapabilities;
    request.effectiveCapabilities = intersectCapabilities(
        protocolCapabilities, request.modelCapabilities, providerCapabilities);
    if (!request.images.empty() && !request.effectiveCapabilities.images) {
        return {aiapi::SubmissionOutcome::InvalidRequest,
                unsupportedCapabilityError("images")};
    }
    if (request.stream && !request.effectiveCapabilities.streaming) {
        return {aiapi::SubmissionOutcome::InvalidRequest,
                unsupportedCapabilityError("streaming")};
    }
    if (!request.toolDefinitions.empty() && !request.effectiveCapabilities.tools) {
        return {aiapi::SubmissionOutcome::InvalidRequest,
                unsupportedCapabilityError("tools")};
    }
    if (request.parallelToolCalls && !request.effectiveCapabilities.parallelTools &&
        !request.toolDefinitions.empty()) {
        request.parallelToolCalls = false;
        request.capabilityDegradations.push_back(
            "parallel_tools_sequentialized_by_provider_capability");
    }

    const std::string responseModel = request.model;
    const bool nativeResponsesToolItems =
        request.clientInfo.isObject() &&
        request.clientInfo.get("client_type", "").asString() == "Codex";
    const int inputTokensEstimated =
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
        protocolOperation + "_generation",
        [request = std::move(request), responseModel,
         nativeResponsesToolItems, inputTokensEstimated,
         binding = std::move(binding), onComplete = std::move(onComplete),
         protocolRegistry = protocolRegistry_,
         responseSinkFactory, protocolOperation,
         providers, sessions, responses, executionGate, channels,
         runtimeConfig = std::move(runtimeConfig)]() mutable {
            std::shared_ptr<IResponseSink> sink;
            try {
                if (!protocolRegistry || !responseSinkFactory) {
                    throw std::runtime_error("Protocol response sink factory is unavailable");
                }
                generation::protocol::ResponseContext context;
                context.operation = protocolOperation;
                context.model = responseModel;
                context.stream = binding.stream;
                context.nativeResponsesToolItems = nativeResponsesToolItems;
                context.inputTokensEstimated = inputTokensEstimated;
                context.jsonResponse = binding.jsonResponse;
                context.streamWriter = binding.streamWriter;
                context.close = binding.close;
                sink = responseSinkFactory->create(context);
            } catch (const std::exception& exception) {
                LOG_ERROR << "[AI use case] response sink construction threw: "
                          << exception.what();
                if (binding.close) binding.close();
                aiapi::GenerationResult result;
                result.error = internalError("Failed to initialize response sink");
                invokeCompletion(onComplete, result, sink);
                return;
            } catch (...) {
                LOG_ERROR << "[AI use case] response sink construction threw an unknown exception";
                if (binding.close) binding.close();
                aiapi::GenerationResult result;
                result.error = internalError("Failed to initialize response sink");
                invokeCompletion(onComplete, result, sink);
                return;
            }

            if (!sink) {
                if (binding.close) binding.close();
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
            } else {
                // Persistence is opt-in through IResponsePersistenceSink; the
                // core does not branch on a protocol operation.
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
