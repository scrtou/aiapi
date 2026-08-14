#include <drogon/drogon_test.h>

#include <infrastructure/provider/ProviderRegistry.h>

#include <memory>

namespace {

class FakeProvider final : public APIinterface
{
  public:
    provider::ProviderResult generate(session_st&) override
    {
        return provider::ProviderResult::success("fake");
    }

    void checkAlivableTokens() override {}
    void checkModels() override {}
    ProviderModelCatalog getModels() override
    {
        ProviderModel model;
        model.id = "fake-model";
        return ProviderModelCatalog{{model}};
    }
    void init() override {}
    void afterResponseProcess(session_st&) override {}
    void eraseChatinfoMap(std::string) override {}
    void transferThreadContext(const std::string&, const std::string&) override {}
};

}  // namespace

DROGON_TEST(ProviderRegistryPort_ResolvesProviderWithoutLegacyLookupCoupling)
{
    provider::ProviderRegistry registry;
    auto provider = std::make_shared<FakeProvider>();
    REQUIRE(registry.registerProvider("fake", provider));

    CHECK(registry.findProvider("fake") == provider);
    CHECK(registry.findProvider("missing") == nullptr);
}

DROGON_TEST(ProviderRegistryPort_FreezesPublishedSnapshot)
{
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerProvider("first", std::make_shared<FakeProvider>()));
    registry.freeze();

    CHECK(registry.isFrozen());
    CHECK(!registry.registerProvider("late", std::make_shared<FakeProvider>()));
    CHECK(registry.findProvider("late") == nullptr);
    CHECK(registry.providerNames() == std::vector<std::string>{"first"});
}
