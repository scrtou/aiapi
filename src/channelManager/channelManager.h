#ifndef CHANNEL_MANAGER_H
#define CHANNEL_MANAGER_H

#include <memory>
#include <list>
#include <optional>
#include <shared_mutex>
#include <drogon/drogon.h>
#include <dbManager/channel/channelDbManager.h>

using std::list;
using std::shared_ptr;
using std::string;

class ChannelManager
{
private:
    shared_ptr<ChannelDbManager> channelDbManager;
    ChannelManager();
    ~ChannelManager();

    // P7: 内存缓存 —— 所有通道信息常驻内存，避免每次请求查 DB
    mutable std::shared_mutex cacheMutex_;
    std::list<Channelinfo_st> channelCache_;

    // / 从 DB 重新加载缓存（需持有 unique_lock）
    void reloadCache();

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

    /// P7： 从内存缓存中查询通道是否支持 工具调用，避免每次请求查数据库
    std::optional<bool> getSupportsToolCalls(const std::string& channelName) const;
};

#endif
