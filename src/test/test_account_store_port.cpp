#include <drogon/drogon_test.h>

#include <application/account/accountManager.h>
#include <domain/port/IAccountStore.h>
#include <domain/port/IChannelStore.h>

#include <list>
#include <memory>
#include <string>

// R4 Pilot C Regressionstest, symmetrisch zu test_channel_store_port.cpp (Pilot B).
//
// Abdeckung bewusst eng: nur der Vertrag zwischen AccountManager und IAccountStore.
// Der Startpfad in main.cc wird NICHT abgedeckt, da der Test selbst injiziert.
// Fehlende Verdrahtung in main.cc bewacht tools/arch/check_startup_wiring.py.
//
// Belastbarkeit (Messergebnis der Schritte 79-83):
// aiapi_test linkt das echte accountManager.cpp, keinen Stub. Wer setStore/requireStore
// kaputt macht, faerbt diese Datei rot. Nur die DB-Kollaboratoren
// (ConfigDbManager / AccountBackupDbManager / RetoolWorkspaceService) werden von
// stub_db_collaborators.cpp zur Linkzeit ersetzt: gestubbt sind die Kollaboratoren,
// nicht das Pruefobjekt.

namespace
{

class FakeAccountStore : public IAccountStore
{
  public:
    int statusByUsernameCalls = 0;
    std::string statusToReturn;
    std::string lastApiName;
    std::string lastUserName;

    bool addAccount(struct Accountinfo_st) override { return true; }
    bool updateAccount(struct Accountinfo_st) override { return true; }
    bool deleteAccount(std::string, std::string) override { return true; }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    std::list<Accountinfo_st> getAccountDBList() override { return {}; }

    int createWaitingAccount(std::string) override { return 0; }
    bool activateAccount(int, struct Accountinfo_st) override { return true; }
    bool deleteWaitingAccount(int) override { return true; }
    // Defaultargument nur einmal in IAccountStore deklariert, hier bewusst ohne.
    int countAccountsByChannel(std::string, bool) override { return 0; }
    bool updateAccountStatusById(int, std::string) override { return true; }
    std::string getAccountStatusByUsername(std::string apiName, std::string userName) override
    {
        ++statusByUsernameCalls;
        lastApiName = apiName;
        lastUserName = userName;
        return statusToReturn;
    }
};

}  // namespace

// Lesezugriff muss vollstaendig ueber den injizierten Store laufen.
DROGON_TEST(AccountStorePortReadUsesInjectedStore)
{
    auto fake = std::make_shared<FakeAccountStore>();
    fake->statusToReturn = AccountStatus::REGISTERING;
    AccountManager manager;
    manager.setStore(fake);

    CHECK(manager.isAccountRegisteringByUsername("__port_user__") == true);
    // Nicht nur der Rueckgabewert: der Aufruf muss wirklich beim Fake ankommen.
    CHECK(fake->statusByUsernameCalls == 1);
    CHECK(fake->lastUserName == "__port_user__");
    CHECK(fake->lastApiName == "chaynsapi");

    // Der Wert wird tatsaechlich uebernommen, nicht zufaellig gleich.
    fake->statusToReturn = "active";
    CHECK(manager.isAccountRegisteringByUsername("__port_user__") == false);
    CHECK(fake->statusByUsernameCalls == 2);
}

// Ohne Injektion: sichere Degradierung auf die Null-Implementierung.
// Pendant zu ChannelStorePortFallsBackWhenNotInjected aus Pilot B.
DROGON_TEST(AccountStorePortFallsBackWhenNotInjected)
{
    AccountManager manager;
    manager.setStore(nullptr);
    CHECK(manager.isAccountRegisteringByUsername("__no_store__") == false);
}


// ---------------------------------------------------------------------------
// R4 试点 C 续：AccountManager 对 ChannelDbManager 的直呼倒置（步骤 98）。
//
// 改造前 accountManager.cpp 有 3 处 ChannelDbManager::getInstance() 直呼。
// 复用已有的 IChannelStore 端口，无需新造抽象。
//
// 断言范围刻意收窄，理由：
// checkChannelAccountCount 在「渠道已找到且启用」的分支会调用 autoRegisterAccount，
// 那是真实网络注册且含 sleep(5s)，单测不可进入。
// 故 Fake 返回空列表，强制走「未找到渠道」的早退分支——
// 该分支在 getChannelList() 之后立即 return，既安全又足以证明依赖来源已被倒置。
// 本用例不覆盖补注册业务逻辑；那需要能拦截 autoRegisterAccount 的接缝，尚不具备。
namespace
{

class FakeChannelStoreForAccount : public IChannelStore
{
  public:
    int listCalls = 0;
    std::list<Channelinfo_st> rows;   // 刻意留空：强制走「未找到渠道」早退分支

    bool addChannel(struct Channelinfo_st) override { return true; }
    bool updateChannel(struct Channelinfo_st) override { return true; }
    bool deleteChannel(int) override { return true; }
    bool getChannel(std::string, struct Channelinfo_st&) override { return false; }
    std::list<Channelinfo_st> getChannelList() override
    {
        ++listCalls;
        return rows;
    }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    bool updateChannelStatus(std::string, bool) override { return true; }
};

}  // namespace

// 渠道列表必须来自注入的 store，而非 ChannelDbManager 单例。
DROGON_TEST(AccountManagerUsesInjectedChannelStore)
{
    auto fake = std::make_shared<FakeChannelStoreForAccount>();
    AccountManager manager;
    manager.setChannelStore(fake);

    // 空列表 -> 必定走「未找到渠道」早退，不会触达 autoRegisterAccount。
    manager.checkChannelAccountCount("__kein_kanal__");

    CHECK(fake->listCalls == 1);
}

// 未注入 channelStore 时安全退化：不崩溃，且不误入补注册路径。
DROGON_TEST(AccountManagerChannelStoreFallsBackWhenNotInjected)
{
    AccountManager manager;
    manager.setChannelStore(nullptr);
    manager.checkChannelAccountCount("__kein_kanal__");
    CHECK(true);   // 走到这里即证明未崩溃
}
