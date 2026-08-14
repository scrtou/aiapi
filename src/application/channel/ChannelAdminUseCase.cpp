#include <application/channel/ChannelAdminUseCase.h>

#include <domain/policy/RetiredProviderPolicy.h>
#include <domain/port/IAccountCatalog.h>
#include <domain/port/IBackgroundExecutor.h>
#include <domain/port/IChannelCatalog.h>

#include <utility>
namespace {

bool isBuiltInChannelName(const std::string& name)
{
    return name == "chaynsapi" || name == "retoolapi";
}

bool isRetired(const Channelinfo_st& channel)
{
    return retired_provider::isRetiredProviderKey(channel.channelName) ||
           retired_provider::isRetiredProviderKey(channel.channelType);
}

ChannelAdminResult failed(Channelinfo_st channel)
{
    ChannelAdminResult result;
    result.channel = std::move(channel);
    return result;
}

}  // namespace

ChannelAdminUseCase::ChannelAdminUseCase(IChannelCatalog* channels,
                                         IAccountCatalog* accounts,
                                         IBackgroundExecutor* executor)
    : channels_(channels), accounts_(accounts), executor_(executor)
{
}

std::list<Channelinfo_st> ChannelAdminUseCase::listChannels() const
{
    return channels_ ? channels_->listChannels() : std::list<Channelinfo_st>{};
}

TaskSubmitResult ChannelAdminUseCase::scheduleAccountRecount(
    const std::string& operation, const std::string& channelName) const
{
    if (!executor_ || !accounts_) {
        return TaskSubmitResult::Stopped;
    }

    auto* accounts = accounts_;
    return executor_->submit(
        "channel" + operation + "_checkCounts_" + channelName,
        [accounts, channelName] { accounts->checkChannelAccountCount(channelName); });
}

ChannelAdminResult ChannelAdminUseCase::add(Channelinfo_st channel)
{
    if (isRetired(channel)) {
        auto result = failed(std::move(channel));
        result.outcome = ChannelAdminOutcome::ProviderRetired;
        return result;
    }
    if (!channels_ || !channels_->addChannel(channel)) {
        return failed(std::move(channel));
    }

    ChannelAdminResult result;
    result.outcome = ChannelAdminOutcome::Success;
    result.channel = std::move(channel);
    result.recountSubmission = scheduleAccountRecount("Add", result.channel.channelName);
    return result;
}

ChannelAdminResult ChannelAdminUseCase::update(Channelinfo_st channel)
{
    if (isRetired(channel)) {
        auto result = failed(std::move(channel));
        result.outcome = ChannelAdminOutcome::ProviderRetired;
        return result;
    }
    if (!channels_) {
        auto result = failed(std::move(channel));
        result.outcome = ChannelAdminOutcome::ServiceUnavailable;
        return result;
    }

    if (isBuiltInChannelName(channel.channelName)) {
        bool found = false;
        for (const auto& existing : channels_->listChannels()) {
            if (existing.channelName != channel.channelName) {
                continue;
            }
            channel.channelType = existing.channelType;
            channel.channelUrl = existing.channelUrl;
            channel.channelKey = existing.channelKey;
            found = true;
            break;
        }
        if (!found) {
            auto result = failed(std::move(channel));
            result.outcome = ChannelAdminOutcome::BuiltInChannelNotFound;
            return result;
        }
    }

    if (!channels_->updateChannel(channel)) {
        return failed(std::move(channel));
    }

    ChannelAdminResult result;
    result.outcome = ChannelAdminOutcome::Success;
    result.channel = std::move(channel);
    result.recountSubmission = scheduleAccountRecount("Update", result.channel.channelName);
    return result;
}

ChannelAdminResult ChannelAdminUseCase::remove(int channelId)
{
    ChannelAdminResult result;
    result.channel.id = channelId;
    if (channels_ && channels_->deleteChannel(channelId)) {
        result.outcome = ChannelAdminOutcome::Success;
    }
    return result;
}

ChannelAdminResult ChannelAdminUseCase::updateStatus(std::string channelName, bool status)
{
    ChannelAdminResult result;
    result.channel.channelName = std::move(channelName);
    result.channel.channelStatus = status;
    if (retired_provider::isRetiredProviderKey(result.channel.channelName)) {
        result.outcome = ChannelAdminOutcome::ProviderRetired;
        return result;
    }
    if (channels_ && channels_->updateChannelStatus(
                         result.channel.channelName, result.channel.channelStatus)) {
        result.outcome = ChannelAdminOutcome::Success;
    }
    return result;
}
