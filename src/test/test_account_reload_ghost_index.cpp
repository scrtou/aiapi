#include <drogon/drogon_test.h>

#include <accountManager/accountManager.h>
#include <domain/port/IAccountStore.h>

#include <list>
#include <memory>
#include <string>

// Regressionstest fuer den Ghost-Index-Defekt in AccountManager::loadAccount().
//
// Defekt (Schritte 232-237): loadAccount() leerte nur accountPoolMap, nie accountList.
// loadAccountFromDatebase() ueberspringt trial-budget-exceeded Datensaetze per continue
// (Backup + Loeschen in der Hauptdatenbank), ruft also kein addAccount(). Ein bereits
// indizierter Account blieb dadurch dauerhaft in accountList, obwohl die DB-Zeile weg war.
// getAccountList() lieferte den Geistereintrag an alle Konsumenten (u. a. account_count
// der Health-Probe) aus.
//
// AccountManager ist ein Singleton (ctor/dtor privat), daher laeuft der Test wie
// test_account_store_port.cpp ueber getInstance(). Jeder Testfall setzt seinen eigenen
// Store und startet mit einem vollen loadAccount(), ist also nicht von Vorlaeufern abhaengig.
//
// Der Test injiziert ueber setStore() und faehrt zwei Ladezyklen: erst ein normaler
// Account, dann derselbe Account als trial_budget_exceeded. Vor dem Fix ist die zweite
// Assertion rot.

namespace
{

Accountinfo_st makeAccount(const std::string& accountType)
{
    Accountinfo_st account;
    account.apiName = "nexosapi";
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
DROGON_TEST(AccountReloadDropsArchivedGhostIndexEntry)
{
    auto store = std::make_shared<ReloadFakeAccountStore>();
    auto& manager = AccountManager::getInstance();
    manager.setStore(store);

    store->rows = {makeAccount("free")};
    manager.loadAccount();

    auto afterFirstLoad = manager.getAccountList();
    REQUIRE(afterFirstLoad.find("nexosapi") != afterFirstLoad.end());
    CHECK(afterFirstLoad["nexosapi"].find("ghost@example.com") != afterFirstLoad["nexosapi"].end());

    store->rows = {makeAccount("trial_budget_exceeded")};
    manager.loadAccount();

    CHECK(store->deleteCalls == 1);
    CHECK(store->lastDeletedUserName == "ghost@example.com");

    auto afterReload = manager.getAccountList();
    auto apiIt = afterReload.find("nexosapi");
    const bool ghostGone = apiIt == afterReload.end() ||
                           apiIt->second.find("ghost@example.com") == apiIt->second.end();
    CHECK(ghostGone);
}

// Leere Datenbank nach einem gefuellten Zyklus darf keinen Rest im Index hinterlassen.
DROGON_TEST(AccountReloadWithEmptyDatabaseClearsIndex)
{
    auto store = std::make_shared<ReloadFakeAccountStore>();
    auto& manager = AccountManager::getInstance();
    manager.setStore(store);

    store->rows = {makeAccount("free")};
    manager.loadAccount();
    REQUIRE(manager.getAccountList().empty() == false);

    store->rows.clear();
    manager.loadAccount();

    auto afterReload = manager.getAccountList();
    auto apiIt = afterReload.find("nexosapi");
    const bool indexEmpty = apiIt == afterReload.end() || apiIt->second.empty();
    CHECK(indexEmpty);
}
