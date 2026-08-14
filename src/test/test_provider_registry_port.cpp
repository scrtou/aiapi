#include <drogon/drogon_test.h>

#include <infrastructure/provider/ProviderBase.h>
#include <infrastructure/provider/ProviderRegistry.h>
#include <sessionManager/core/Session.h>

#include <memory>
#include <utility>
#include <vector>

namespace {

class NarrowProvider final : public provider::ProviderBase,
                             public provider::IProviderModelCatalog,
                             public provider::IProviderThreadContext
{
  public:
    provider::ProviderCapabilities capabilities() const noexcept override
    {
        return provider::ProviderCapabilities{/*nativeToolCalls=*/false,
                                              /*upstreamHistory=*/true,
                                              /*supportsImages=*/false};
    }

    ProviderModelCatalog getModels() override
    {
        ProviderModel model;
        model.id = "narrow-model";
        return ProviderModelCatalog{{model}};
    }

    platform::Result<void> eraseThreadContext(const std::string& conversationId) override
    {
        erasedConversationIds.push_back(conversationId);
        return platform::Result<void>::success();
    }

    platform::Result<void> transferThreadContext(const std::string& oldId,
                                                  const std::string& newId) override
    {
        transfers.emplace_back(oldId, newId);
        return platform::Result<void>::success();
    }

    platform::Result<void> deleteUpstreamThread(const std::string&,
                                                 const std::string&,
                                                 const std::string&,
                                                 const std::string&) override
    {
        return platform::Result<void>::success();
    }

    std::vector<std::string> erasedConversationIds;
    std::vector<std::pair<std::string, std::string>> transfers;

  protected:
    platform::Result<provider::ProviderResponse> doGenerate(
        const provider::ProviderRequest&,
        provider::ProviderCallContext&) override
    {
        provider::ProviderResponse response;
        response.text = "fake";
        return platform::Result<provider::ProviderResponse>::success(std::move(response));
    }

    std::string_view providerName() const noexcept override { return "narrow"; }
};

}  // namespace

DROGON_TEST(ProviderRegistryPort_ResolvesProviderWithoutLegacyLookupCoupling)
{
    provider::ProviderRegistry registry;
    auto provider = std::make_shared<NarrowProvider>();
    REQUIRE(registry.registerChatProvider("fake", provider));

    CHECK(registry.findChatProvider("fake") == provider);
    CHECK(registry.findChatProvider("missing") == nullptr);
}

DROGON_TEST(ProviderRegistryPort_FreezesPublishedSnapshot)
{
    provider::ProviderRegistry registry;
    REQUIRE(registry.registerChatProvider("first", std::make_shared<NarrowProvider>()));
    registry.freeze();

    CHECK(registry.isFrozen());
    CHECK(!registry.registerChatProvider("late", std::make_shared<NarrowProvider>()));
    CHECK(registry.findChatProvider("late") == nullptr);
    CHECK(registry.providerNames() == std::vector<std::string>{"first"});
}

DROGON_TEST(ProviderRegistryPort_ResolvesNarrowCapabilitiesWithoutLegacyFallback)
{
    provider::ProviderRegistry registry;
    auto narrow = std::make_shared<NarrowProvider>();
    REQUIRE(registry.registerChatProvider("chaynsapi", narrow, narrow, narrow));

    CHECK(registry.findChatProvider("chaynsapi") == narrow);
    CHECK(registry.findModelCatalog("chaynsapi") == narrow);
    CHECK(registry.findThreadContext("chaynsapi") == narrow);
    CHECK(!registry.registerChatProvider("chaynsapi", std::make_shared<NarrowProvider>()));
    CHECK(registry.providerNames() == std::vector<std::string>{"chaynsapi"});
}

DROGON_TEST(ProviderRegistryPort_SessionUsesNarrowThreadContextForTransferAndCleanup)
{
    provider::ProviderRegistry registry;
    auto narrow = std::make_shared<NarrowProvider>();
    REQUIRE(registry.registerChatProvider("chaynsapi", narrow, nullptr, narrow));

    chatSession sessions;
    sessions.setProviderRegistry(&registry);

    session_st responseSession;
    responseSession.state.conversationId = "response-conversation";
    responseSession.request.api = "chaynsapi";
    sessions.addSession(responseSession.state.conversationId, responseSession);

    session_st continuation = responseSession;
    continuation.state.isContinuation = true;
    continuation.provider.prevProviderKey = "previous-provider-conversation";
    sessions.updateResponseSession(continuation);

    REQUIRE(narrow->transfers.size() == 1);
    CHECK(narrow->transfers[0].first == "previous-provider-conversation");
    CHECK(narrow->transfers[0].second == "response-conversation");

    REQUIRE(sessions.deleteResponseSession("response-conversation"));
    CHECK(narrow->erasedConversationIds ==
          std::vector<std::string>{"response-conversation"});
}
