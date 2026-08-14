#pragma once

#include <domain/port/IChannelAdminUseCase.h>

class IAccountCatalog;
class IBackgroundExecutor;
class IChannelCatalog;

class ChannelAdminUseCase final : public IChannelAdminUseCase
{
  public:
    ChannelAdminUseCase(IChannelCatalog* channels,
                        IAccountCatalog* accounts,
                        IBackgroundExecutor* executor);

    std::list<Channelinfo_st> listChannels() const override;
    ChannelAdminResult add(Channelinfo_st channel) override;
    ChannelAdminResult update(Channelinfo_st channel) override;
    ChannelAdminResult remove(int channelId) override;
    ChannelAdminResult updateStatus(std::string channelName, bool status) override;

  private:
    TaskSubmitResult scheduleAccountRecount(const std::string& operation,
                                            const std::string& channelName) const;

    IChannelCatalog* channels_;
    IAccountCatalog* accounts_;
    IBackgroundExecutor* executor_;
};
