#include <drogon/drogon_test.h>

#include <accountManager/accountManager.h>
#include <domain/port/IAccountStore.h>

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
    AccountManager::getInstance().setStore(fake);

    CHECK(AccountManager::getInstance().isAccountRegisteringByUsername("__port_user__") == true);
    // Nicht nur der Rueckgabewert: der Aufruf muss wirklich beim Fake ankommen.
    CHECK(fake->statusByUsernameCalls == 1);
    CHECK(fake->lastUserName == "__port_user__");
    CHECK(fake->lastApiName == "chaynsapi");

    // Der Wert wird tatsaechlich uebernommen, nicht zufaellig gleich.
    fake->statusToReturn = "active";
    CHECK(AccountManager::getInstance().isAccountRegisteringByUsername("__port_user__") == false);
    CHECK(fake->statusByUsernameCalls == 2);
}

// Ohne Injektion: sichere Degradierung auf die Null-Implementierung.
// Pendant zu ChannelStorePortFallsBackWhenNotInjected aus Pilot B.
DROGON_TEST(AccountStorePortFallsBackWhenNotInjected)
{
    AccountManager::getInstance().setStore(nullptr);
    CHECK(AccountManager::getInstance().isAccountRegisteringByUsername("__no_store__") == false);
}
