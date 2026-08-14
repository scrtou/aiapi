#include <drogon/drogon_test.h>

#include <application/channel/ChannelAdminUseCase.h>
#include <domain/port/IAccountCatalog.h>
#include <domain/port/IChannelCatalog.h>

// ARCH_TESTS: application/channel/ChannelAdminUseCase.h
// ARCH_TESTS: domain/port/IChannelAdminUseCase.h

namespace {

class ChannelCatalog final : public IChannelCatalog
{
  public:
    std::list<Channelinfo_st> rows;
    Channelinfo_st updated;
    int addCalls = 0;
    int updateCalls = 0;

    std::list<Channelinfo_st> listChannels() const override { return rows; }
    bool addChannel(Channelinfo_st) override { ++addCalls; return true; }
    bool updateChannel(Channelinfo_st channel) override
    {
        ++updateCalls;
        updated = std::move(channel);
        return true;
    }
    bool deleteChannel(int) override { return true; }
    bool updateChannelStatus(std::string, bool) override { return true; }
    std::optional<bool> supportsToolCalls(const std::string&) const override
    { return std::nullopt; }
};

class Accounts final : public IAccountCatalog
{
  public:
    int recountCalls = 0;
    std::string lastChannel;

    AccountMap listAccounts() override { return {}; }
    void checkChannelAccountCount(std::string channelName) override
    {
        ++recountCalls;
        lastChannel = std::move(channelName);
    }
};

class InlineExecutor final : public IBackgroundExecutor
{
  public:
    std::string lastName;
    TaskSubmitResult result = TaskSubmitResult::Accepted;

    TaskSubmitResult submit(const std::string& name, std::function<void()> task) override
    {
        lastName = name;
        if (result == TaskSubmitResult::Accepted) task();
        return result;
    }
};

}  // namespace

DROGON_TEST(ChannelAdminUseCasePreservesBuiltInFieldsAndSchedulesRecount)
{
    ChannelCatalog channels;
    Accounts accounts;
    InlineExecutor executor;
    Channelinfo_st existing;
    existing.id = 42;
    existing.channelName = "chaynsapi";
    existing.channelType = "chaynsapi";
    existing.channelUrl = "https://fixed.invalid";
    existing.channelKey = "fixed-key";
    channels.rows.push_back(existing);

    ChannelAdminUseCase useCase(&channels, &accounts, &executor);
    Channelinfo_st incoming;
    incoming.id = existing.id;
    incoming.channelName = "chaynsapi";
    incoming.channelType = "caller-must-not-change";
    incoming.channelUrl = "https://caller.invalid";
    incoming.channelKey = "caller-key";

    const auto result = useCase.update(incoming);
    CHECK(result.succeeded());
    CHECK(channels.updateCalls == 1);
    CHECK(channels.updated.channelType == "chaynsapi");
    CHECK(channels.updated.channelUrl == "https://fixed.invalid");
    CHECK(channels.updated.channelKey == "fixed-key");
    CHECK(result.recountSubmission == TaskSubmitResult::Accepted);
    CHECK(accounts.recountCalls == 1);
    CHECK(accounts.lastChannel == "chaynsapi");
    CHECK(executor.lastName == "channelUpdate_checkCounts_chaynsapi");
}

DROGON_TEST(ChannelAdminUseCaseRejectsRetiredProviderBeforePersistence)
{
    ChannelCatalog channels;
    Accounts accounts;
    InlineExecutor executor;
    ChannelAdminUseCase useCase(&channels, &accounts, &executor);

    Channelinfo_st retired;
    retired.channelName = "nexosapi";
    const auto result = useCase.add(retired);

    CHECK(result.outcome == ChannelAdminOutcome::ProviderRetired);
    CHECK(channels.addCalls == 0);
    CHECK(accounts.recountCalls == 0);
}
