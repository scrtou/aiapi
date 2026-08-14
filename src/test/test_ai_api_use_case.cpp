#include <drogon/drogon_test.h>

#include <sessionManager/core/AiApiUseCase.h>

// ARCH_TESTS: domain/model/AiApiData.h
// ARCH_TESTS: domain/port/IAiApiUseCase.h

#include <memory>
#include <string>
#include <unordered_map>

namespace {

class FakeProvider final : public APIinterface
{
  public:
    provider::ProviderResult generate(session_st&) override
    {
        return provider::ProviderResult::success("unused");
    }
    void checkAlivableTokens() override {}
    void checkModels() override {}
    ProviderModelCatalog getModels() override
    {
        ProviderModel model;
        model.id = "facade-model";
        return ProviderModelCatalog{{model}};
    }
    void init() override {}
    void afterResponseProcess(session_st&) override {}
    void eraseChatinfoMap(std::string) override {}
    void transferThreadContext(const std::string&, const std::string&) override {}
};

class FakeProviderRegistry final : public IProviderRegistry
{
  public:
    std::shared_ptr<APIinterface> provider;

    std::shared_ptr<APIinterface> findProvider(const std::string&) const override
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
    providers.provider = std::make_shared<FakeProvider>();
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
