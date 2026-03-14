#include "channelManager.h"

namespace {

bool isBuiltInChannelName(const std::string& name)
{
    return name == "chaynsapi" || name == "nexosapi";
}

std::list<Channelinfo_st> buildDefaultChannels()
{
    return {
        Channelinfo_st(
            "chaynsapi",
            "chaynsapi",
            "",
            "",
            true,
            10,
            30,
            0,
            "Built-in channel: chaynsapi",
            0,
            false
        ),
        Channelinfo_st(
            "nexosapi",
            "nexosapi",
            "",
            "",
            true,
            10,
            30,
            0,
            "Built-in channel: nexosapi",
            0,
            false
        )
    };
}

}

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

    // 确保内置渠道存在：渠道名固定，默认目标账号数为 0，默认不支持工具调用
    for (const auto& channel : buildDefaultChannels()) {
        Channelinfo_st existing;
        if (!channelDbManager->getChannel(channel.channelName, existing)) {
            if (channelDbManager->addChannel(channel)) {
                LOG_INFO << "[渠道管理] 已自动生成内置渠道: " << channel.channelName;
            } else {
                LOG_WARN << "[渠道管理] 自动生成内置渠道失败: " << channel.channelName;
            }
        }
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
    {
        std::shared_lock<std::shared_mutex> lock(cacheMutex_);
        for (const auto& channel : channelCache_) {
            if (channel.id == channelId && isBuiltInChannelName(channel.channelName)) {
                LOG_WARN << "[渠道管理] 拒绝删除内置渠道: " << channel.channelName;
                return false;
            }
        }
    }

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
