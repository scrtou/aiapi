#include <drogon/drogon_test.h>

#include <accountManager/accountManager.h>
#include <domain/port/IAccountStore.h>

#include <list>
#include <memory>
#include <string>

// Regressionstest fuer den Ghost-Index-Defekt in AccountManager::loadAccount().
//
// Defekt (Schritte 232-237): loadAccount() leerte nur accountPoolMap, nie accountList.
// Verschwand ein zuvor geladener Datensatz aus der Datenbank, blieb er deshalb dauerhaft
// in accountList, obwohl der neue Ladezyklus kein addAccount() mehr fuer ihn aufrief.
// getAccountList() lieferte den Geistereintrag an alle Konsumenten (u. a. account_count
// der Health-Probe) aus.
//
// AccountManager ist ein explizit konstruiertes Lifecycle-Objekt. Jeder Testfall
// besitzt daher seinen eigenen Manager und Store und ist nicht von Vorlaeufern abhaengig.
//
// Der Test injiziert ueber setStore() und faehrt zwei Ladezyklen: erst mit einem Account,
// dann mit leerer Datenbank. Vor dem Fix ist die zweite Assertion rot.

namespace
{

Accountinfo_st makeAccount(const std::string& accountType)
{
    Accountinfo_st account;
    account.apiName = "test-provider";
    account.userName = "ghost@example.com";
    account.passwd = "pw";
    account.authToken = "tok";
    account.useCount = 0;
    account.tokenStatus = true;
    account.accountStatus = true;
    account.userTobitId = 0;
    account.personId = "P-1";
    account.createTime = "2026-01-01 00:00:00";
    account.accountType = accountType;
    account.status = "active";
    account.workspaceUacId = 0;
    return account;
}

class ReloadFakeAccountStore : public IAccountStore
{
  public:
    std::list<Accountinfo_st> rows;
    int deleteCalls = 0;
    std::string lastDeletedUserName;

    bool addAccount(struct Accountinfo_st) override { return true; }
    bool updateAccount(struct Accountinfo_st) override { return true; }
    bool deleteAccount(std::string, std::string userName) override
    {
        ++deleteCalls;
        lastDeletedUserName = userName;
        rows.clear();
        return true;
    }
    bool isTableExist() override { return true; }
    void createTable() override {}
    void checkAndUpgradeTable() override {}
    std::list<Accountinfo_st> getAccountDBList() override { return rows; }

    int createWaitingAccount(std::string) override { return 0; }
    bool activateAccount(int, struct Accountinfo_st) override { return true; }
    bool deleteWaitingAccount(int) override { return true; }
    int countAccountsByChannel(std::string, bool) override { return 0; }
    bool updateAccountStatusById(int, std::string) override { return true; }
    std::string getAccountStatusByUsername(std::string, std::string) override { return "active"; }
};

}  // namespace

// Erster Ladezyklus indiziert, zweiter Ladezyklus muss den Eintrag wieder entfernen.
DROGON_TEST(AccountReloadDropsRemovedDatabaseRowFromIndex)
{
    auto store = std::make_shared<ReloadFakeAccountStore>();
    AccountManager manager;
    manager.setStore(store);

    store->rows = {makeAccount("free")};
    manager.loadAccount();

    auto afterFirstLoad = manager.getAccountList();
    REQUIRE(afterFirstLoad.find("test-provider") != afterFirstLoad.end());
    CHECK(afterFirstLoad["test-provider"].find("ghost@example.com") != afterFirstLoad["test-provider"].end());

    store->rows.clear();
    manager.loadAccount();

    CHECK(store->deleteCalls == 0);

    auto afterReload = manager.getAccountList();
    auto apiIt = afterReload.find("test-provider");
    const bool ghostGone = apiIt == afterReload.end() ||
                           apiIt->second.find("ghost@example.com") == apiIt->second.end();
    CHECK(ghostGone);
}

// Leere Datenbank nach einem gefuellten Zyklus darf keinen Rest im Index hinterlassen.
DROGON_TEST(AccountReloadWithEmptyDatabaseClearsIndex)
{
    auto store = std::make_shared<ReloadFakeAccountStore>();
    AccountManager manager;
    manager.setStore(store);

    store->rows = {makeAccount("free")};
    manager.loadAccount();
    REQUIRE(manager.getAccountList().empty() == false);

    store->rows.clear();
    manager.loadAccount();

    auto afterReload = manager.getAccountList();
    auto apiIt = afterReload.find("test-provider");
    const bool indexEmpty = apiIt == afterReload.end() || apiIt->second.empty();
    CHECK(indexEmpty);
}
