#ifndef DBMANAGER_H
#define DBMANAGER_H

#include <drogon/drogon.h>
#include <string>
#include <list>
#include <memory>
#include <domain/model/AccountData.h>
#include <domain/port/IAccountStore.h>
#include <dbManager/DbType.h>

using std::list;
using std::shared_ptr;
using std::string;

class AccountDbManager : public IAccountStore
{
    public:
    // The composition root owns this concrete store.  Construction must stay
    // side-effect free so test fixtures cannot accidentally initialize Drogon.
    AccountDbManager() = default;
    AccountDbManager(const AccountDbManager&) = delete;
    AccountDbManager& operator=(const AccountDbManager&) = delete;

    /// Bind the configured DB client before the injected IAccountStore is used.
    bool initialize(std::string* errorMessage = nullptr);
    void init();
    bool addAccount(struct Accountinfo_st accountinfo) override;
    bool updateAccount(struct Accountinfo_st accountinfo) override;
    bool deleteAccount(string apiName,string userName) override;
    bool getAccount(struct Accountinfo_st accountinfo);
    bool saveAccount(struct Accountinfo_st accountinfo);
    bool saveAccountList(list<struct Accountinfo_st> accountList);
    bool isTableExist() override;
    void createTable() override;
    void checkAndUpgradeTable() override;
    list<Accountinfo_st> getAccountDBList() override;
    DbType getDbType() const { return dbType; }
    
    // 状态预占相关方法
    int createWaitingAccount(string apiName) override;  // 创建待注册占位记录，返回记录ID
    bool activateAccount(int waitingId, struct Accountinfo_st accountinfo) override;  // 激活待注册账号
    bool deleteWaitingAccount(int waitingId) override;  // 删除待注册账号
    int countAccountsByChannel(string apiName, bool includeWaiting) override;  // 默认值只在 IAccountStore 声明  // 统计渠道账号数
    bool updateAccountStatus(string apiName, string userName, string status);  // 更新账号状态
    bool updateAccountStatusById(int id, string status) override;  // 根据ID更新账号状态
    string getAccountStatusById(int id);  // 根据ID获取账号状态
    string getAccountStatusByUsername(string apiName, string userName) override;  // 根据用户名获取账号状态
    
    private:
    void detectDbType();
    shared_ptr<drogon::orm::DbClient> dbClient;
    DbType dbType = DbType::PostgreSQL;
    bool initialized_ = false;
    
};

#endif
