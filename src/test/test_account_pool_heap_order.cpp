#include <drogon/drogon_test.h>

#include <accountManager/accountManager.h>
#include <domain/port/IAccountStore.h>

#include <list>
#include <memory>
#include <string>

// Regressionstest fuer die verletzte Heap-Invariante von accountPoolMap (Schritte 256-260).
//
// AccountCompare sortiert nach (tokenStatus, useCount). Beide Felder sind veraenderlich und
// werden ueber die in accountList geteilten shared_ptr mutiert - dieselben Objekte liegen
// gleichzeitig in der priority_queue. std::priority_queue bemerkt Aenderungen im Inneren
// ihrer Elemente nicht, die Heap-Ordnung bleibt also auf dem alten Schluessel stehen.
//
// Nachgewiesen mit einem Minimalexperiment: nach A->useCount = 100 und A->tokenStatus = false
// liefert top() weiterhin A, obwohl B mit useCount 5 und gueltigem Token vorne stehen muesste.
//
// Folgen: setStatusTokenStatus() entwertet ein Konto, die Queue sortiert es aber nicht um.
// getAccount() nimmt top() ohne jede Gueltigkeitspruefung - ein beliebiger Provider bekommt dadurch ein
// entwertetes Konto und liefert nullptr, obwohl weiter unten im Heap ein gueltiges liegt.

namespace
{

Accountinfo_st makeAccount(const std::string& userName, int useCount)
{
    Accountinfo_st account;
    account.apiName = "test-provider";
    account.userName = userName;
    account.passwd = "pw";
    account.authToken = "tok-" + userName;
    account.useCount = useCount;
    account.tokenStatus = true;
    account.accountStatus = true;
    account.userTobitId = 0;
    account.personId = "P-" + userName;
    account.createTime = "2026-01-01 00:00:00";
    account.accountType = "free";
    account.status = "active";
    account.workspaceUacId = 0;
    return account;
}

Accountinfo_st makeTypedAccount(const std::string& userName, int useCount, const std::string& accountType)
{
    auto account = makeAccount(userName, useCount);
    account.accountType = accountType;
    return account;
}

class HeapOrderFakeAccountStore : public IAccountStore
{
  public:
    std::list<Accountinfo_st> rows;

    bool addAccount(struct Accountinfo_st) override { return true; }
    bool updateAccount(struct Accountinfo_st) override { return true; }
    bool deleteAccount(std::string, std::string) override { return true; }
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

// Ein per setStatusTokenStatus() entwertetes Konto darf nicht mehr vor einem gueltigen stehen.
DROGON_TEST(AccountPoolReordersAfterTokenStatusInvalidation)
{
    auto store = std::make_shared<HeapOrderFakeAccountStore>();
    auto& manager = AccountManager::getInstance();
    manager.setStore(store);

    // low@ hat useCount 0 und steht damit initial an der Spitze des Heaps.
    store->rows = {makeAccount("low@example.com", 0), makeAccount("high@example.com", 5)};
    manager.loadAccount();

    std::shared_ptr<Accountinfo_st> first;
    manager.getAccount("test-provider", first, "");
    REQUIRE(first != nullptr);
    CHECK(first->userName == "low@example.com");

    // Spitzenkonto entwerten: der Heap muss danach high@ ausliefern.
    manager.setStatusTokenStatus("test-provider", "low@example.com", false);

    std::shared_ptr<Accountinfo_st> second;
    manager.getAccount("test-provider", second, "");
    REQUIRE(second != nullptr);
    CHECK(second->tokenStatus == true);
    CHECK(second->userName == "high@example.com");
}

// updateAccount() darf die Heap-Ordnung nicht stehen lassen, wenn es useCount hochsetzt.
DROGON_TEST(AccountPoolReordersAfterUseCountUpdate)
{
    auto store = std::make_shared<HeapOrderFakeAccountStore>();
    auto& manager = AccountManager::getInstance();
    manager.setStore(store);

    store->rows = {makeAccount("low@example.com", 0), makeAccount("high@example.com", 5)};
    manager.loadAccount();

    // low@ auf useCount 100 heben: der faire Kandidat ist danach high@.
    auto bumped = makeAccount("low@example.com", 100);
    REQUIRE(manager.updateAccount(bumped));

    std::shared_ptr<Accountinfo_st> selected;
    manager.getAccount("test-provider", selected, "");
    REQUIRE(selected != nullptr);
    CHECK(selected->userName == "high@example.com");
}

// Belastungsprobe fuer den accountType-Zweig von getAccount().
// Nachgewiesenes Verhalten (nicht Vermutung): Der Zweig entnimmt Eintraege per pop() und legt
// sie per push() zurueck. Beide Operationen ordnen nach dem aktuellen Schluessel und beheben
// flache Ordnungsfehler nebenbei. Ein einzelnes entwertetes Konto in einem kleinen Heap wird
// dadurch auch ohne Re-Heapify korrekt uebersprungen - ein so kleines Szenario ist als Test
// also wertlos, weil es immer gruen bleibt.
// Erst mehrere gleichzeitig entwertete Konten mit hoher Prioritaet in einem groesseren Heap
// ueberschreiten diese Selbstheilung: ohne Re-Heapify liefert getAccount() hier ein
// entwertetes Konto aus (verifiziert gegen den Stand vor dem Fix).
// Szenario daher nicht verkleinern und Anzahl der Entwertungen nicht reduzieren.
DROGON_TEST(AccountPoolTypeFilterUnderMultipleInvalidations)
{
    auto store = std::make_shared<HeapOrderFakeAccountStore>();
    auto& manager = AccountManager::getInstance();
    manager.setStore(store);

    store->rows = {makeTypedAccount("free-0@example.com", 0, "free"),
                   makeTypedAccount("free-1@example.com", 1, "free"),
                   makeTypedAccount("free-2@example.com", 2, "free"),
                   makeTypedAccount("free-3@example.com", 3, "free"),
                   makeTypedAccount("free-4@example.com", 4, "free"),
                   makeTypedAccount("free-5@example.com", 5, "free"),
                   makeTypedAccount("pro-x@example.com", 0, "pro")};
    manager.loadAccount();

    manager.setStatusTokenStatus("test-provider", "free-0@example.com", false);
    manager.setStatusTokenStatus("test-provider", "free-1@example.com", false);
    manager.setStatusTokenStatus("test-provider", "free-2@example.com", false);

    for (int i = 0; i < 3; ++i)
    {
        std::shared_ptr<Accountinfo_st> picked;
        manager.getAccount("test-provider", picked, "free");
        REQUIRE(picked != nullptr);
        CHECK(picked->tokenStatus == true);
    }
}
