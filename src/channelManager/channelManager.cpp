#include "channelManager.h"

ChannelManager::ChannelManager()
{
}

ChannelManager::~ChannelManager()
{
}

void ChannelManager::reloadCache()
{
    // 调用方必须持有 unique_lock(cacheMutex_)
    channelCache_ = channelDbManager->getChannelList();
}

void ChannelManager::init()
{
    LOG_INFO << "[渠道管理] 初始化开始";
    channelDbManager = ChannelDbManager::getInstance();
    if (!channelDbManager->isTableExist())
    {
        channelDbManager->createTable();
    }
    else
    {
        channelDbManager->checkAndUpgradeTable();
    }
    // P7: 初始化时加载通道列表到内存缓存
    {
        std::unique_lock<std::shared_mutex> lock(cacheMutex_);
        reloadCache();
    }
    LOG_INFO << "[渠道管理] 初始化完成，已加载渠道数：" << channelCache_.size();
}

bool ChannelManager::addChannel(struct Channelinfo_st channelinfo)
{
    bool ok = channelDbManager->addChannel(channelinfo);
    if (ok) {
        std::unique_lock<std::shared_mutex> lock(cacheMutex_);
        reloadCache();
    }
    return ok;
}

bool ChannelManager::updateChannel(struct Channelinfo_st channelinfo)
{
    bool ok = channelDbManager->updateChannel(channelinfo);
    if (ok) {
        std::unique_lock<std::shared_mutex> lock(cacheMutex_);
        reloadCache();
    }
    return ok;
}

bool ChannelManager::deleteChannel(int channelId)
{
    bool ok = channelDbManager->deleteChannel(channelId);
    if (ok) {
        std::unique_lock<std::shared_mutex> lock(cacheMutex_);
        reloadCache();
    }
    return ok;
}

list<Channelinfo_st> ChannelManager::getChannelList()
{
    // P7: 从内存缓存返回，不再查 DB
    std::shared_lock<std::shared_mutex> lock(cacheMutex_);
    return channelCache_;
}

bool ChannelManager::updateChannelStatus(string channelName, bool status)
{
    bool ok = channelDbManager->updateChannelStatus(channelName, status);
    if (ok) {
        std::unique_lock<std::shared_mutex> lock(cacheMutex_);
        reloadCache();
    }
    return ok;
}

std::optional<bool> ChannelManager::getSupportsToolCalls(const std::string& channelName) const
{
    // P7: 从内存缓存中查找，零 DB 开销
    std::shared_lock<std::shared_mutex> lock(cacheMutex_);
    for (const auto& ch : channelCache_) {
        if (ch.channelName == channelName) {
            return ch.supportsToolCalls;
        }
    }
    return std::nullopt;
}
