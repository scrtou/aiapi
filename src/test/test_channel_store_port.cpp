#include <drogon/drogon_test.h>

#include <channelManager/channelManager.h>
#include <domain/port/IChannelStore.h>

#include <list>
#include <memory>
#include <string>

// R4 依赖倒置试点 B 的回归测试。
//
// 覆盖范围（须明确，避免高估）：本文件只验证 ChannelManager 与 IChannelStore
// 之间的契约——注入的实现是否被真正使用、未注入时是否安全退化。
// 它不覆盖 main.cc 的接线：用例自行注入 Fake，不经过启动路径。
// 「main.cc 漏调 setStore」这一真实事故由 tools/arch/check_startup_wiring.py
// 静态检查守卫（退出码 4）。两者互补，缺一不可。
//
// 另一层意义：改造前 ChannelManager 在 init() 里直取 ChannelDbManager 单例，
// 本文件根本无法存在——测它就得连上真实数据库。现在只需注入内存 Fake，
// CMake 里也无需链接任何 dbManager 源文件。

namespace
{

class FakeChannelStore : public IChannelStore
{
  public:
    int addCalls = 0;
    int listCalls = 0;
    int createTableCalls = 0;
    std::list<Channelinfo_st> rows;

    bool addChannel(struct Channelinfo_st c) override
    {
        ++addCalls;
        rows.push_back(c);
        return true;
    }
    bool updateChannel(struct Channelinfo_st) override { return true; }
    bool deleteChannel(int) override { return true; }
    bool getChannel(std::string name, struct Channelinfo_st& out) override
    {
        for (const auto& r : rows)
        {
            if (r.channelName == name)
            {
                out = r;
                return true;
            }
        }
        return false;
    }
    std::list<Channelinfo_st> getChannelList() override
    {
        ++listCalls;
        return rows;
    }
    bool isTableExist() override { return false; }
    void createTable() override { ++createTableCalls; }
    void checkAndUpgradeTable() override {}
    bool updateChannelStatus(std::string, bool) override { return true; }
};

}  // namespace

// init() 必须完全经由注入的 store 工作，不得自取具体实现。
DROGON_TEST(ChannelStorePortInitUsesInjectedStore)
{
    auto fake = std::make_shared<FakeChannelStore>();
    ChannelManager manager;
    manager.setStore(fake);
    manager.init();

    // isTableExist 返回 false，init 应据此建表。
    CHECK(fake->createTableCalls == 1);
    // 内置渠道应通过注入的 store 写入。
    CHECK(fake->addCalls > 0);
    // 缓存加载应经由注入的 store 读取。
    CHECK(fake->listCalls > 0);
}

// 业务写操作应转发到同一个 store。
DROGON_TEST(ChannelStorePortAddChannelForwards)
{
    auto fake = std::make_shared<FakeChannelStore>();
    ChannelManager manager;
    manager.setStore(fake);

    const int before = fake->addCalls;
    Channelinfo_st c;
    c.channelName = "__port_test_channel__";
    CHECK(manager.addChannel(c) == true);
    CHECK(fake->addCalls == before + 1);
}

// 未注入时应退化为 Null 实现：不崩溃，且返回失败而非静默成功。
// 这条正是 main.cc 漏注入场景的守门断言。
DROGON_TEST(ChannelStorePortFallsBackWhenNotInjected)
{
    ChannelManager manager;
    manager.setStore(nullptr);
    Channelinfo_st c;
    c.channelName = "__no_store__";
    CHECK(manager.addChannel(c) == false);
    CHECK(manager.getChannelList().empty());
}
