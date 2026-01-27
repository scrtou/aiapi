#ifndef DBMANAGER_H
#define DBMANAGER_H

#include <drogon/drogon.h>
#include <string>
#include <list>
#include <memory>
#include <accountManager/accountManager.h>
using namespace drogon;

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
    
    private:
    void detectDbType();
    shared_ptr<drogon::orm::DbClient> dbClient;
    DbType dbType = DbType::PostgreSQL;  // 默认为 PostgreSQL
    
};

#endif
