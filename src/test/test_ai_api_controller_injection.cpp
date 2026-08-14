#include <drogon/drogon_test.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <controllers/AiApiController.h>

// ARCH_TESTS: domain/model/AiApiData.h
// ARCH_TESTS: domain/port/IAiApiUseCase.h

namespace {

class FakeAiApiUseCase final : public aiapi::IAiApiUseCase
{
  public:
    int modelCalls = 0;
    int getCalls = 0;
    int deleteCalls = 0;
    std::string lastProvider;
    std::string lastResponseId;

    aiapi::SubmissionResult submitGeneration(
        aiapi::GenerationInput, SinkFactory, Completion) override
    {
        aiapi::SubmissionResult result;
        result.outcome = aiapi::SubmissionOutcome::Accepted;
        return result;
    }

    aiapi::ModelCatalogResult modelCatalog(const std::string& provider) const override
    {
        auto* self = const_cast<FakeAiApiUseCase*>(this);
        ++self->modelCalls;
        self->lastProvider = provider;
        aiapi::ModelCatalogResult result;
        result.outcome = aiapi::ModelCatalogOutcome::Found;
        ProviderModel model;
        model.id = "injected-model";
        result.catalog.models.push_back(std::move(model));
        return result;
    }

    aiapi::StoredResponseResult getResponse(const std::string& responseId) override
    {
        ++getCalls;
        lastResponseId = responseId;
        aiapi::StoredResponseResult result;
        result.outcome = aiapi::StoredResponseOutcome::Found;
        result.jsonBody = R"({"id":"resp_injected","object":"response"})";
        return result;
    }

    aiapi::DeleteResponseResult deleteResponse(const std::string& responseId) override
    {
        ++deleteCalls;
        lastResponseId = responseId;
        aiapi::DeleteResponseResult result;
        result.outcome = aiapi::DeleteResponseOutcome::Deleted;
        return result;
    }
};

}  // namespace

DROGON_TEST(AiApiControllerUsesItsInjectedUseCaseForModelsAndResponses)
{
    FakeAiApiUseCase useCase;
    AiApiController::setUseCase(&useCase);
    AiApiController controller;

    auto modelRequest = drogon::HttpRequest::newHttpRequest();
    modelRequest->setPath("/retoolapi/v1/models");
    drogon::HttpResponsePtr captured;
    controller.chaynsapimodels(
        modelRequest,
        [&captured](const drogon::HttpResponsePtr& response) { captured = response; });

    REQUIRE(captured != nullptr);
    const auto models = captured->getJsonObject();
    REQUIRE(models != nullptr);
    CHECK(useCase.modelCalls == 1);
    CHECK(useCase.lastProvider == "retoolapi");
    CHECK((*models)["data"][0]["id"].asString() == "injected-model");

    captured.reset();
    controller.responsesGet(
        drogon::HttpRequest::newHttpRequest(),
        [&captured](const drogon::HttpResponsePtr& response) { captured = response; },
        "resp_injected");
    REQUIRE(captured != nullptr);
    const auto stored = captured->getJsonObject();
    REQUIRE(stored != nullptr);
    CHECK(useCase.getCalls == 1);
    CHECK(useCase.lastResponseId == "resp_injected");
    CHECK((*stored)["id"].asString() == "resp_injected");

    captured.reset();
    controller.responsesDelete(
        drogon::HttpRequest::newHttpRequest(),
        [&captured](const drogon::HttpResponsePtr& response) { captured = response; },
        "resp_injected");
    REQUIRE(captured != nullptr);
    const auto deleted = captured->getJsonObject();
    REQUIRE(deleted != nullptr);
    CHECK(useCase.deleteCalls == 1);
    CHECK((*deleted)["deleted"].asBool());

    AiApiController::setUseCase(nullptr);
}
