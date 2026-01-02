#ifndef CHANNEL_MANAGER_H
#define CHANNEL_MANAGER_H

#include <memory>
#include <list>
#include <drogon/drogon.h>
#include <dbManager/channel/channelDbManager.h>

using namespace std;
using namespace drogon;

class ChannelManager
{
private:
    shared_ptr<ChannelDbManager> channelDbManager;
    ChannelManager();
    ~ChannelManager();

public:
    static ChannelManager& getInstance()
    {
        static ChannelManager instance;
        return instance;
    }

    ChannelManager(const ChannelManager&) = delete;
    ChannelManager& operator=(const ChannelManager&) = delete;

    void init();

    bool addChannel(struct Channelinfo_st channelinfo);
    bool updateChannel(struct Channelinfo_st channelinfo);
    bool deleteChannel(int channelId);
    list<Channelinfo_st> getChannelList();
    bool updateChannelStatus(string channelName, bool status);
};

#endif