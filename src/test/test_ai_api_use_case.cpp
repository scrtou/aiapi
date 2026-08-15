#include <drogon/drogon_test.h>

#include <application/generation/core/AiApiUseCase.h>

// ARCH_TESTS: domain/model/AiApiData.h
// ARCH_TESTS: domain/port/IAiApiUseCase.h

#include <memory>
#include <string>
#include <unordered_map>

namespace {

class FakeProviderCatalog final : public provider::IProviderModelCatalog
{
  public:
    bool supportsImages = false;

    ProviderModelCatalog getModels() override
    {
        ProviderModel model;
        model.id = "facade-model";
        ChaynsModelExtension extension;
        extension.capabilities.images = supportsImages;
        model.chayns = std::move(extension);
        return ProviderModelCatalog{{model}};
    }

    std::optional<ProviderModelCapabilities> findModelCapabilities(
        const std::string& modelId) const override
    {
        if (modelId != "facade-model") return std::nullopt;
        ProviderModelCapabilities capabilities;
        capabilities.images = supportsImages;
        return capabilities;
    }
};

class FakeChatProvider final : public provider::IChatProvider
{
  public:
    platform::Result<provider::ProviderResponse> generate(
        const provider::ProviderRequest&,
        provider::ProviderCallContext&) override
    {
        return platform::Result<provider::ProviderResponse>::failure(
            platform::Error::internal("unused fake provider"));
    }

    provider::ProviderCapabilities capabilities() const noexcept override
    {
        return provider::ProviderCapabilities{/*nativeToolCalls=*/false,
                                              /*upstreamHistory=*/true,
                                              /*supportsImages=*/true};
    }
};

class FakeProviderRegistry final : public IProviderRegistry
{
  public:
    std::shared_ptr<provider::IProviderModelCatalog> provider;
    std::shared_ptr<provider::IChatProvider> chatProvider;

    std::shared_ptr<provider::IChatProvider> findChatProvider(const std::string&) const override
    {
        return chatProvider;
    }

    std::shared_ptr<provider::IProviderModelCatalog> findModelCatalog(
        const std::string&) const override
    {
        return provider;
    }
};

class FakeResponseIndex final : public IResponseIndex
{
  public:
    std::unordered_map<std::string, std::string> responses;

    bool tryGetSessionId(const std::string&, std::string&) override { return false; }
    void bind(const std::string&, const std::string&) override {}
    bool tryGetResponse(const std::string& responseId, std::string& output) override
    {
        const auto it = responses.find(responseId);
        if (it == responses.end()) return false;
        output = it->second;
        return true;
    }
    void storeResponse(const std::string& responseId, const std::string& json) override
    {
        responses[responseId] = json;
    }
    bool erase(const std::string& responseId) override
    {
        return responses.erase(responseId) != 0;
    }
    void cleanup(size_t, std::chrono::seconds) override {}
};

}  // namespace

DROGON_TEST(AiApiUseCaseOwnsCatalogAndResponsesIndexWorkflows)
{
    FakeProviderRegistry providers;
    providers.provider = std::make_shared<FakeProviderCatalog>();
    FakeResponseIndex responses;
    responses.responses["resp_ok"] = R"({"id":"resp_ok","object":"response"})";
    responses.responses["resp_corrupt"] = "not json";

    AiApiUseCase useCase(&providers, nullptr, &responses, nullptr, nullptr, nullptr);

    const auto models = useCase.modelCatalog("chaynsapi");
    REQUIRE(models.found());
    REQUIRE(models.catalog.models.size() == 1);
    CHECK(models.catalog.models[0].id == "facade-model");

    const auto found = useCase.getResponse("resp_ok");
    CHECK(found.outcome == aiapi::StoredResponseOutcome::Found);
    CHECK(found.jsonBody.find("resp_ok") != std::string::npos);
    CHECK(useCase.getResponse("missing").outcome == aiapi::StoredResponseOutcome::NotFound);
    CHECK(useCase.getResponse("resp_corrupt").outcome == aiapi::StoredResponseOutcome::Corrupt);

    CHECK(useCase.deleteResponse("resp_ok").deleted());
    CHECK(!useCase.deleteResponse("resp_ok").deleted());
}

DROGON_TEST(AiApiUseCase_RejectsCapabilityMissingFromDeclaredModel)
{
    FakeProviderRegistry providers;
    auto catalog = std::make_shared<FakeProviderCatalog>();
    catalog->supportsImages = false;
    providers.provider = catalog;
    providers.chatProvider = std::make_shared<FakeChatProvider>();

    AiApiUseCase useCase(&providers, nullptr, nullptr, nullptr, nullptr, nullptr);
    aiapi::GenerationInput input;
    input.provider = "fake";
    input.method = "POST";
    input.path = "/v1/chat/completions";
    input.jsonBody = R"({
        "model":"facade-model",
        "messages":[{
            "role":"user",
            "content":[{"type":"image_url","image_url":{"url":"https://example.invalid/a.png"}}]
        }]
    })";

    const auto result = useCase.submitGeneration(input, {}, {});
    CHECK(result.outcome == aiapi::SubmissionOutcome::InvalidRequest);
    REQUIRE(result.error.has_value());
    CHECK(result.error->type == "unsupported_capability");
    CHECK(result.error->code == "unsupported_images");
}
