#include "accountDbManager.h"
#include <algorithm>  // for std::transform
//pg create table
std::string tableName = "account";
std::string createTablePgSql = R"(
    CREATE TABLE IF NOT EXISTS account (
        id SERIAL PRIMARY KEY,
        updatetime TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        createtime TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        apiname VARCHAR(255),
        username VARCHAR(255),
        password VARCHAR(255),
        authtoken TEXT,
        usecount INTEGER,
        tokenstatus BOOLEAN,
        accountstatus BOOLEAN,
        usertobitid INTEGER,
        personid VARCHAR(255),
        accounttype VARCHAR(50) DEFAULT 'free'
    );
)";
std::string createTableSqlMysql=R"(
    CREATE TABLE IF NOT EXISTS account (
    id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    updatetime DATETIME DEFAULT CURRENT_TIMESTAMP,
    createtime DATETIME DEFAULT CURRENT_TIMESTAMP,
    apiname VARCHAR(255),
    username VARCHAR(255),
    password VARCHAR(255),
    authtoken TEXT,
    usecount INT,
    tokenstatus TINYINT(1),
    accountstatus TINYINT(1),
    usertobitid INT,
    personid VARCHAR(255),
    accounttype VARCHAR(50) DEFAULT 'free'
) ENGINE=InnoDB;)";

std::string createTableSqlite3 = R"(
    CREATE TABLE IF NOT EXISTS account (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        updatetime DATETIME DEFAULT CURRENT_TIMESTAMP,
        createtime DATETIME DEFAULT CURRENT_TIMESTAMP,
        apiname TEXT,
        username TEXT,
        password TEXT,
        authtoken TEXT,
        usecount INTEGER,
        tokenstatus INTEGER,
        accountstatus INTEGER,
        usertobitid INTEGER,
        personid TEXT,
        accounttype TEXT DEFAULT 'free'
    );
)";


void AccountDbManager::detectDbType()
{
    // 从配置文件的 custom_config 读取数据库类型
    auto customConfig = drogon::app().getCustomConfig();
    std::string dbTypeStr = "postgresql";  // 默认值
    
    if (customConfig.isMember("dbtype")) {
        dbTypeStr = customConfig["dbtype"].asString();
    }
    
    // 转换为小写进行比较
    std::transform(dbTypeStr.begin(), dbTypeStr.end(), dbTypeStr.begin(), ::tolower);
    
    if (dbTypeStr == "sqlite3" || dbTypeStr == "sqlite") {
        dbType = DbType::SQLite3;
        LOG_INFO << "[账户数据库] 配置的数据库类型: SQLite3";
    } else if (dbTypeStr == "mysql" || dbTypeStr == "mariadb") {
        dbType = DbType::MySQL;
        LOG_INFO << "[账户数据库] 配置的数据库类型: MySQL";
    } else {
        dbType = DbType::PostgreSQL;
        LOG_INFO << "[账户数据库] 配置的数据库类型: PostgreSQL";
    }
}

void AccountDbManager::init()
{
    LOG_INFO << "[账户数据库] 初始化开始";
    dbClient = app().getDbClient("aichatpg");
    detectDbType();
    LOG_INFO << "[账户数据库] 初始化完成";
}

void AccountDbManager::checkAndUpgradeTable()
{
    bool hasAccountType = false;
    
    if (dbType == DbType::SQLite3)
    {
        // SQLite3: 使用 PRAGMA table_info
        std::string checkSql = "PRAGMA table_info(account)";
        auto result = dbClient->execSqlSync(checkSql);
        for (const auto& row : result)
        {
            if (row["name"].as<std::string>() == "accounttype")
            {
                hasAccountType = true;
                break;
            }
        }
    }
    else
    {
        // PostgreSQL/MySQL: 使用 information_schema
        std::string checkSql = "SELECT column_name FROM information_schema.columns WHERE table_name='account' AND column_name='accounttype'";
        auto result = dbClient->execSqlSync(checkSql);
        hasAccountType = (result.size() > 0);
    }
    
    if (!hasAccountType)
    {
        LOG_INFO << "[账户数据库] 表'account'中缺少列'accounttype', 正在添加...";
        try {
            if (dbType == DbType::SQLite3) {
                dbClient->execSqlSync("ALTER TABLE account ADD COLUMN accounttype TEXT DEFAULT 'free'");
            } else {
                dbClient->execSqlSync("ALTER TABLE account ADD COLUMN accounttype VARCHAR(50) DEFAULT 'free'");
            }
            LOG_INFO << "[账户数据库] 列'accounttype'添加成功";
        } catch(const std::exception& e) {
            LOG_ERROR << "[账户数据库] 添加列'accounttype'失败: " << e.what();
        }
    }
}

bool AccountDbManager::addAccount(struct Accountinfo_st accountinfo)
{
    std::string selectsql = "select * from account where apiname=$1 and username=$2";
    std::string insertsql = "insert into account (apiname,username,password,authtoken,usecount,tokenstatus,accountstatus,usertobitid,personid,createtime,accounttype) values ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)";
    std::string updatesql = "update account set password=$1,authtoken=$2,usecount=$3,tokenstatus=$4,accountstatus=$5,usertobitid=$6,personid=$7,accounttype=$8 where apiname=$9 and username=$10";
    auto result = dbClient->execSqlSync(selectsql,accountinfo.apiName,accountinfo.userName);
    if(result.size()!=0)
    {
        LOG_INFO << "账号 " << accountinfo.userName << " 已存在,更新账号";
        return updateAccount(accountinfo);
    }
    else
    {
         auto result1 =dbClient->execSqlSync(insertsql,accountinfo.apiName,accountinfo.userName,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId,accountinfo.createTime,accountinfo.accountType);
        if(result1.affectedRows()!=0)
        {
            LOG_INFO << "账号 " << accountinfo.userName << " 添加成功";
            return true;
        }
        else
        {
            LOG_ERROR << "账号 " << accountinfo.userName << " 添加失败";
            return false;
        }
    }
}
bool AccountDbManager::updateAccount(struct Accountinfo_st accountinfo)
{
    std::string updatesql = "update account set password=$1,authtoken=$2,usecount=$3,tokenstatus=$4,accountstatus=$5,usertobitid=$6,personid=$7,accounttype=$8 where apiname=$9 and username=$10";
    auto result = dbClient->execSqlSync(updatesql,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId,accountinfo.accountType,accountinfo.apiName,accountinfo.userName);
    if(result.affectedRows()!=0)
    {
        LOG_INFO << "账号 " << accountinfo.userName << " 更新成功";
        return true;
    }
    else
    {
        LOG_ERROR << "账号 " << accountinfo.userName << " 更新失败";
        return false;
    }
    
}
bool AccountDbManager::deleteAccount(string apiName,string userName)
{
    std::string deletesql = "delete from account where apiname=$1 and username=$2";
    auto result = dbClient->execSqlSync(deletesql,apiName,userName);
    if(result.affectedRows()!=0)
    {
        LOG_INFO << "账号 " << userName << " 删除成功";
        return true;
    }
    else
    {
        LOG_ERROR << "账号 " << userName << " 删除失败";
        return false;
    }
}
list<Accountinfo_st> AccountDbManager::getAccountDBList()
{
    std::string selectsql = "select apiname,username,password,authtoken,usecount,tokenstatus,accountstatus,usertobitid,personid,createtime,COALESCE(accounttype,'free') as accounttype from account";
    auto result = dbClient->execSqlSync(selectsql);
    list<Accountinfo_st> accountDBList;
    for(auto& item:result)
    {
        std::string createTimeStr = "";
        if (!item["createtime"].isNull()) {
            createTimeStr = item["createtime"].as<std::string>();
        }
        std::string accountTypeStr = "free";
        if (!item["accounttype"].isNull()) {
            accountTypeStr = item["accounttype"].as<std::string>();
        }
        Accountinfo_st accountinfo(item["apiname"].as<std::string>(),item["username"].as<std::string>(),item["password"].as<std::string>(),item["authtoken"].as<std::string>(),item["usecount"].as<int>(),item["tokenstatus"].as<bool>(),item["accountstatus"].as<bool>(),item["usertobitid"].as<int>(),item["personid"].as<std::string>(),createTimeStr,accountTypeStr);
        accountDBList.push_back(accountinfo);
    }
    return accountDBList;
}
bool AccountDbManager::isTableExist()
{
    if (dbType == DbType::SQLite3)
    {
        // SQLite3: 使用 sqlite_master
        auto result = dbClient->execSqlSync("SELECT name FROM sqlite_master WHERE type='table' AND name='" + tableName + "'");
        return result.size() != 0;
    }
    else
    {
        // PostgreSQL/MySQL: 使用 information_schema
        auto result = dbClient->execSqlSync("SELECT table_name FROM information_schema.tables WHERE table_name='" + tableName + "'");
        return result.size() != 0;
    }
}

void AccountDbManager::createTable()
{
    try
    {
        if (dbType == DbType::SQLite3) {
            dbClient->execSqlSync(createTableSqlite3);
        } else if (dbType == DbType::MySQL) {
            dbClient->execSqlSync(createTableSqlMysql);
        } else {
            dbClient->execSqlSync(createTablePgSql);
        }
        LOG_INFO << "[账户数据库] 账户表创建成功";
    }
    catch(const std::exception& e)
    {
        LOG_ERROR << "[账户数据库] 创建表错误: " << e.what();
    }
}