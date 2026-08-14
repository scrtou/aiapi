#include <drogon/drogon_test.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <controllers/ChannelController.h>

// ARCH_TESTS: domain/port/IChannelAdminUseCase.h

namespace {

class FakeChannelAdminUseCase final : public IChannelAdminUseCase
{
  public:
    std::list<Channelinfo_st> channels;
    ChannelAdminResult addResult;
    ChannelAdminResult updateResult;
    ChannelAdminResult removeResult;
    ChannelAdminResult statusResult;
    int updateCalls = 0;

    std::list<Channelinfo_st> listChannels() const override { return channels; }
    ChannelAdminResult add(Channelinfo_st channel) override
    {
        auto result = addResult;
        if (result.channel.channelName.empty()) result.channel = std::move(channel);
        return result;
    }
    ChannelAdminResult update(Channelinfo_st channel) override
    {
        ++updateCalls;
        auto result = updateResult;
        if (result.channel.channelName.empty()) result.channel = std::move(channel);
        return result;
    }
    ChannelAdminResult remove(int channelId) override
    {
        auto result = removeResult;
        result.channel.id = channelId;
        return result;
    }
    ChannelAdminResult updateStatus(std::string channelName, bool status) override
    {
        auto result = statusResult;
        result.channel.channelName = std::move(channelName);
        result.channel.channelStatus = status;
        return result;
    }
};

}  // namespace

DROGON_TEST(ChannelControllerListsThroughInjectedUseCase)
{
    FakeChannelAdminUseCase channels;
    Channelinfo_st channel;
    channel.channelName = "fake-channel";
    channel.channelType = "fake-type";
    channels.channels.push_back(channel);
    ChannelController::setUseCase(&channels);

    drogon::HttpResponsePtr captured;
    ChannelController controller;
    controller.channelList(
        drogon::HttpRequest::newHttpRequest(),
        [&captured](const drogon::HttpResponsePtr& response) { captured = response; });

    REQUIRE(captured != nullptr);
    const auto json = captured->getJsonObject();
    REQUIRE(json != nullptr);
    CHECK(json->size() == 1);
    CHECK((*json)[0]["channelname"].asString() == "fake-channel");

    ChannelController::setUseCase(nullptr);
}

DROGON_TEST(ChannelControllerBuiltInUpdateWithoutUseCaseDegradesSafely)
{
    ChannelController::setUseCase(nullptr);

    auto request = drogon::HttpRequest::newHttpRequest();
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody("{\"channelname\":\"chaynsapi\"}");
    drogon::HttpResponsePtr captured;
    ChannelController controller;
    controller.channelUpdate(
        request,
        [&captured](const drogon::HttpResponsePtr& response) { captured = response; });

    REQUIRE(captured != nullptr);
    // Missing controller-facing use case has the same safe degraded outcome as
    // the previous missing catalog binding: no dereference and a failed write.
    CHECK(captured->getStatusCode() == drogon::k200OK);
    const auto json = captured->getJsonObject();
    REQUIRE(json != nullptr);
    CHECK((*json)["status"].asString() == "failed");
}

DROGON_TEST(ChannelControllerUsesInjectedUseCaseForBuiltInUpdate)
{
    FakeChannelAdminUseCase channels;
    channels.updateResult.outcome = ChannelAdminOutcome::Success;
    channels.updateResult.channel.id = 7;
    channels.updateResult.channel.channelName = "chaynsapi";
    channels.updateResult.recountSubmission = TaskSubmitResult::Accepted;
    ChannelController::setUseCase(&channels);

    auto request = drogon::HttpRequest::newHttpRequest();
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody("{\"channelname\":\"chaynsapi\",\"channeltype\":\"changed\"}");
    drogon::HttpResponsePtr captured;
    ChannelController controller;
    controller.channelUpdate(
        request,
        [&captured](const drogon::HttpResponsePtr& response) { captured = response; });

    REQUIRE(captured != nullptr);
    CHECK(channels.updateCalls == 1);
    const auto json = captured->getJsonObject();
    REQUIRE(json != nullptr);
    CHECK((*json)["status"].asString() == "success");
    CHECK((*json)["id"].asInt() == 7);

    ChannelController::setUseCase(nullptr);
}
