#ifndef ACCOUNT_BACKUP_DB_MANAGER_H
#define ACCOUNT_BACKUP_DB_MANAGER_H

#include <domain/model/AccountData.h>
#include <domain/port/IAccountBackupStore.h>
#include <drogon/orm/DbClient.h>
#include <list>
#include <memory>
#include <string>

class AccountBackupDbManager : public IAccountBackupStore
{
  public:
    // The composition root owns this local SQLite adapter.  Opening the file
    // is an explicit runtime action rather than a static-construction effect.
    AccountBackupDbManager() = default;
    AccountBackupDbManager(const AccountBackupDbManager&) = delete;
    AccountBackupDbManager& operator=(const AccountBackupDbManager&) = delete;

    bool initialize(std::string* errorMessage = nullptr);

    bool ensureTable();
    bool backupAccount(const Accountinfo_st& accountinfo, const std::string& reason);
    std::list<Accountinfo_st> getBackupAccountList();
    std::list<Accountinfo_st> listBackupAccounts() override
    { return getBackupAccountList(); }

  private:
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    bool initialized_ = false;
};

#endif
