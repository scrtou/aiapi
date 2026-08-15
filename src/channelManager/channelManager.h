#ifndef CHANNEL_MANAGER_H
#define CHANNEL_MANAGER_H

#include <memory>
#include <list>
#include <optional>
#include <shared_mutex>
#include <domain/port/IChannelStore.h>
#include <domain/port/IChannelCatalog.h>

using std::list;
using std::shared_ptr;
using std::string;

class ChannelManager : public IChannelCatalog
{
private:
    shared_ptr<IChannelStore> channelDbManager;

    // P7: 内存缓存 —— 所有通道信息常驻内存，避免每次请求查 DB
    mutable std::shared_mutex cacheMutex_;
    std::list<Channelinfo_st> channelCache_;

    // / 从 DB 重新加载缓存（需持有 unique_lock）
    void reloadCache();

    // / 取 store；未注入时返回 Null 实现，避免空指针崩溃
    IChannelStore* requireStore();

public:
    /// AppContext owns the production instance; tests use independent locals.
    /// There is deliberately no process-global accessor.
    ChannelManager();
    ~ChannelManager();

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
    std::list<Channelinfo_st> listChannels() const override;
    bool updateChannelStatus(string channelName, bool status);

    /// P7： 从内存缓存中查询通道是否支持 工具调用，避免每次请求查数据库
    std::optional<bool> getSupportsToolCalls(const std::string& channelName) const;
    std::optional<bool> supportsToolCalls(const std::string& channelName) const override
    { return getSupportsToolCalls(channelName); }
};

#endif
