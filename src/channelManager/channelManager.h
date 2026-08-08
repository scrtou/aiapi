#ifndef CHANNEL_MANAGER_H
#define CHANNEL_MANAGER_H

#include <memory>
#include <list>
#include <optional>
#include <shared_mutex>
#include <drogon/drogon.h>
#include <domain/port/IChannelStore.h>

using std::list;
using std::shared_ptr;
using std::string;

class ChannelManager
{
private:
    shared_ptr<IChannelStore> channelDbManager;
    ChannelManager();
    ~ChannelManager();

    // P7: 内存缓存 —— 所有通道信息常驻内存，避免每次请求查 DB
    mutable std::shared_mutex cacheMutex_;
    std::list<Channelinfo_st> channelCache_;

    // / 从 DB 重新加载缓存（需持有 unique_lock）
    void reloadCache();

    // / 取 store；未注入时返回 Null 实现，避免空指针崩溃
    IChannelStore* requireStore();

public:
    static ChannelManager& getInstance()
    {
        static ChannelManager instance;
        return instance;
    }

    ChannelManager(const ChannelManager&) = delete;
    ChannelManager& operator=(const ChannelManager&) = delete;

    // / 注入 Channel 持久化实现（R4 依赖倒置试点 B）。
    // / 必须在 init() 之前调用 —— init() 会立刻建表并写入内置渠道。
    void setStore(std::shared_ptr<IChannelStore> store);

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
