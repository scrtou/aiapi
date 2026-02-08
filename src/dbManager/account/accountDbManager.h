#ifndef DBMANAGER_H
#define DBMANAGER_H

#include <drogon/drogon.h>
#include <string>
#include <list>
#include <memory>
#include <accountManager/accountManager.h>

using std::list;
using std::make_shared;
using std::shared_ptr;
using std::string;
using drogon::app;

// 数据库类型枚举
enum class DbType {
    PostgreSQL,
    SQLite3,
    MySQL
};

class AccountDbManager
{
    public:
    static shared_ptr<AccountDbManager> getInstance()
    {
        static shared_ptr<AccountDbManager> instance;
        if(instance == nullptr)
        {
            instance = make_shared<AccountDbManager>();
            instance->dbClient = app().getDbClient("aichatpg");
            instance->detectDbType();
        }
        return instance;
    }
    void init();
    bool addAccount(struct Accountinfo_st accountinfo);
    bool updateAccount(struct Accountinfo_st accountinfo);
    bool deleteAccount(string apiName,string userName);
    bool getAccount(struct Accountinfo_st accountinfo);
    bool saveAccount(struct Accountinfo_st accountinfo);
    bool saveAccountList(list<struct Accountinfo_st> accountList);
    bool isTableExist();
    void createTable();
    void checkAndUpgradeTable();
    list<Accountinfo_st> getAccountDBList();
    DbType getDbType() const { return dbType; }
    
    // 状态预占相关方法
    int createWaitingAccount(string apiName);  // 创建待注册占位记录，返回记录ID
    bool activateAccount(int waitingId, struct Accountinfo_st accountinfo);  // 激活待注册账号
    bool deleteWaitingAccount(int waitingId);  // 删除待注册账号
    int countAccountsByChannel(string apiName, bool includeWaiting = true);  // 统计渠道账号数
    bool updateAccountStatus(string apiName, string userName, string status);  // 更新账号状态
    bool updateAccountStatusById(int id, string status);  // 根据ID更新账号状态
    string getAccountStatusById(int id);  // 根据ID获取账号状态
    string getAccountStatusByUsername(string apiName, string userName);  // 根据用户名获取账号状态
    
    private:
    void detectDbType();
    shared_ptr<drogon::orm::DbClient> dbClient;
    DbType dbType = DbType::PostgreSQL;
    
};

#endif
