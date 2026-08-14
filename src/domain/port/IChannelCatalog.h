#pragma once

#include <domain/model/ChannelInfo.h>

#include <list>
#include <optional>
#include <string>

class IChannelCatalog
{
  public:
    virtual ~IChannelCatalog() = default;
    virtual std::list<Channelinfo_st> listChannels() const = 0;
    virtual bool addChannel(Channelinfo_st channel) = 0;
    virtual bool updateChannel(Channelinfo_st channel) = 0;
    virtual bool deleteChannel(int channelId) = 0;
    virtual bool updateChannelStatus(std::string channelName, bool status) = 0;
    virtual std::optional<bool> supportsToolCalls(const std::string& channelName) const = 0;
};
