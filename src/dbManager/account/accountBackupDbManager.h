#ifndef ACCOUNT_BACKUP_DB_MANAGER_H
#define ACCOUNT_BACKUP_DB_MANAGER_H

#include <accountManager/accountManager.h>
#include <drogon/orm/DbClient.h>
#include <list>
#include <memory>
#include <string>

class AccountBackupDbManager
{
  public:
    static std::shared_ptr<AccountBackupDbManager> getInstance()
    {
        static std::shared_ptr<AccountBackupDbManager> instance =
            std::make_shared<AccountBackupDbManager>();
        return instance;
    }

    AccountBackupDbManager();

    bool ensureTable();
    bool backupAccount(const Accountinfo_st& accountinfo, const std::string& reason);
    std::list<Accountinfo_st> getBackupAccountList();

  private:
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
};

#endif
