#include "channelManager.h"

namespace {

bool isBuiltInChannelName(const std::string& name)
{
    return name == "chaynsapi" || name == "nexosapi" || name == "retoolapi";
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
            0,
            false
        ),
        Channelinfo_st(
            "retoolapi",
            "retool",
            "/retoolapi/v1/chat/completions",
            "",
            true,
            10,
            900,
            0,
            "Built-in channel: retool workspace runtime",
            0,
            0,
            false
        )
    };
}

}

// ---------------------------------------------------------------------------
// R4 依赖倒置支撑代码（试点 B，样式对齐 RetoolWorkspaceManager）
// ---------------------------------------------------------------------------
namespace
{
// 未注入实现时的 Null Object：不崩溃，但留下可诊断日志。
// init() 会立刻建表并写内置渠道，若静默失败会难以排查，故每次调用都告警。
class NullChannelStore : public IChannelStore
{
  public:
    bool addChannel(struct Channelinfo_st) override { return fail(); }
    bool updateChannel(struct Channelinfo_st) override { return fail(); }
    bool deleteChannel(int) override { return fail(); }
    bool getChannel(std::string, struct Channelinfo_st&) override { return fail(); }
    std::list<Channelinfo_st> getChannelList() override
    {
        fail();
        return {};
    }
    bool isTableExist() override { return fail(); }
    void createTable() override { fail(); }
    void checkAndUpgradeTable() override { fail(); }
    bool updateChannelStatus(std::string, bool) override { return fail(); }

  private:
    static bool fail()
    {
        LOG_ERROR << "[渠道管理] store 未注入（应由 main.cc 在 init() 前调 setStore）";
        return false;
    }
};
}  // namespace

void ChannelManager::setStore(std::shared_ptr<IChannelStore> store)
{
    channelDbManager = std::move(store);
}

IChannelStore* ChannelManager::requireStore()
{
    if (channelDbManager != nullptr)
    {
        return channelDbManager.get();
    }
    static NullChannelStore nullStore;
    return &nullStore;
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
    channelCache_ = requireStore()->getChannelList();
}

void ChannelManager::init()
{
    LOG_INFO << "[渠道管理] 初始化开始";
    if (!requireStore()->isTableExist())
    {
        requireStore()->createTable();
    }
    else
    {
        requireStore()->checkAndUpgradeTable();
    }

    // 确保内置渠道存在：渠道名固定，默认目标账号数为 0，默认不支持工具调用
    for (const auto& channel : buildDefaultChannels()) {
        Channelinfo_st existing;
        if (!requireStore()->getChannel(channel.channelName, existing)) {
            if (requireStore()->addChannel(channel)) {
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
    bool ok = requireStore()->addChannel(channelinfo);
    if (ok) {
        std::unique_lock<std::shared_mutex> lock(cacheMutex_);
        reloadCache();
    }
    return ok;
}

bool ChannelManager::updateChannel(struct Channelinfo_st channelinfo)
{
    bool ok = requireStore()->updateChannel(channelinfo);
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

    bool ok = requireStore()->deleteChannel(channelId);
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
    bool ok = requireStore()->updateChannelStatus(channelName, status);
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
