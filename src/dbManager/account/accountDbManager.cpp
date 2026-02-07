#include "accountDbManager.h"
#include <algorithm>  // for std::transform
#include <chrono>     // for std::chrono in createPendingAccount
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
        accounttype VARCHAR(50) DEFAULT 'free',
        status VARCHAR(20) DEFAULT 'active'
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
    accounttype VARCHAR(50) DEFAULT 'free',
    status VARCHAR(20) DEFAULT 'active'
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
        accounttype TEXT DEFAULT 'free',
        status TEXT DEFAULT 'active'
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
    bool hasStatus = false;
    
    if (dbType == DbType::SQLite3)
    {
        // SQLite3: 使用 PRAGMA table_info
        std::string checkSql = "PRAGMA table_info(account)";
        auto result = dbClient->execSqlSync(checkSql);
        for (const auto& row : result)
        {
            std::string colName = row["name"].as<std::string>();
            if (colName == "accounttype")
            {
                hasAccountType = true;
            }
            if (colName == "status")
            {
                hasStatus = true;
            }
        }
    }
    else
    {
        // PostgreSQL/MySQL: 使用 information_schema
        std::string checkSql = "SELECT column_name FROM information_schema.columns WHERE table_name='account' AND column_name='accounttype'";
        auto result = dbClient->execSqlSync(checkSql);
        hasAccountType = (result.size() > 0);
        
        std::string checkStatusSql = "SELECT column_name FROM information_schema.columns WHERE table_name='account' AND column_name='status'";
        auto resultStatus = dbClient->execSqlSync(checkStatusSql);
        hasStatus = (resultStatus.size() > 0);
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
    
    if (!hasStatus)
    {
        LOG_INFO << "[账户数据库] 表'account'中缺少列'status', 正在添加...";
        try {
            if (dbType == DbType::SQLite3) {
                dbClient->execSqlSync("ALTER TABLE account ADD COLUMN status TEXT DEFAULT 'active'");
            } else {
                dbClient->execSqlSync("ALTER TABLE account ADD COLUMN status VARCHAR(20) DEFAULT 'active'");
            }
            LOG_INFO << "[账户数据库] 列'status'添加成功";
        } catch(const std::exception& e) {
            LOG_ERROR << "[账户数据库] 添加列'status'失败: " << e.what();
        }
    }
}

bool AccountDbManager::addAccount(struct Accountinfo_st accountinfo)
{
    std::string selectsql = "select * from account where apiname=$1 and username=$2";

    // 注意：PostgreSQL 的 TIMESTAMP 不接受空字符串 ''。
    // 当 createTime 为空时，让数据库使用列默认值（CURRENT_TIMESTAMP），以兼容 SQLite/PG/MySQL。
    std::string insertsqlWithCreateTime = "insert into account (apiname,username,password,authtoken,usecount,tokenstatus,accountstatus,usertobitid,personid,createtime,accounttype,status) values ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)";
    std::string insertsqlNoCreateTime   = "insert into account (apiname,username,password,authtoken,usecount,tokenstatus,accountstatus,usertobitid,personid,accounttype,status) values ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)";

    std::string updatesql = "update account set password=$1,authtoken=$2,usecount=$3,tokenstatus=$4,accountstatus=$5,usertobitid=$6,personid=$7,accounttype=$8,status=$9 where apiname=$10 and username=$11";

    // 如果status为空，默认设置为active
    if (accountinfo.status.empty()) {
        accountinfo.status = AccountStatus::ACTIVE;
    }

    auto result = dbClient->execSqlSync(selectsql, accountinfo.apiName, accountinfo.userName);
    if (result.size() != 0)
    {
        LOG_INFO << "账号 " << accountinfo.userName << " 已存在,更新账号";
        return updateAccount(accountinfo);
    }
    else
    {
        auto result1 = [&]() {
            if (accountinfo.createTime.empty())
            {
                return dbClient->execSqlSync(
                    insertsqlNoCreateTime,
                    accountinfo.apiName,
                    accountinfo.userName,
                    accountinfo.passwd,
                    accountinfo.authToken,
                    accountinfo.useCount,
                    accountinfo.tokenStatus,
                    accountinfo.accountStatus,
                    accountinfo.userTobitId,
                    accountinfo.personId,
                    accountinfo.accountType,
                    accountinfo.status);
            }
            return dbClient->execSqlSync(
                insertsqlWithCreateTime,
                accountinfo.apiName,
                accountinfo.userName,
                accountinfo.passwd,
                accountinfo.authToken,
                accountinfo.useCount,
                accountinfo.tokenStatus,
                accountinfo.accountStatus,
                accountinfo.userTobitId,
                accountinfo.personId,
                accountinfo.createTime,
                accountinfo.accountType,
                accountinfo.status);
        }();

        if (result1.affectedRows() != 0)
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
    std::string updatesql = "update account set password=$1,authtoken=$2,usecount=$3,tokenstatus=$4,accountstatus=$5,usertobitid=$6,personid=$7,accounttype=$8,status=$9 where apiname=$10 and username=$11";
    auto result = dbClient->execSqlSync(updatesql,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId,accountinfo.accountType,accountinfo.status,accountinfo.apiName,accountinfo.userName);
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
    std::string selectsql = "select apiname,username,password,authtoken,usecount,tokenstatus,accountstatus,usertobitid,personid,createtime,COALESCE(accounttype,'free') as accounttype,COALESCE(status,'active') as status from account";
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
        std::string statusStr = AccountStatus::ACTIVE;
        if (!item["status"].isNull()) {
            statusStr = item["status"].as<std::string>();
        }
        Accountinfo_st accountinfo(item["apiname"].as<std::string>(),item["username"].as<std::string>(),item["password"].as<std::string>(),item["authtoken"].as<std::string>(),item["usecount"].as<int>(),item["tokenstatus"].as<bool>(),item["accountstatus"].as<bool>(),item["usertobitid"].as<int>(),item["personid"].as<std::string>(),createTimeStr,accountTypeStr,statusStr);
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

// ==================== 状态预占相关方法 ====================

int AccountDbManager::createWaitingAccount(string apiName)
{
    LOG_INFO << "[账户数据库] 创建待注册账号: " << apiName;
    
    // 生成一个临时的占位用户名
    std::string waitingUsername = "waiting_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

    // waiting 账号不提供 createtime，让数据库使用默认值（CURRENT_TIMESTAMP），兼容 SQLite/PG/MySQL。
    std::string insertsql = "insert into account (apiname,username,password,authtoken,usecount,tokenstatus,accountstatus,usertobitid,personid,accounttype,status) values ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)";

    try {
        auto result = dbClient->execSqlSync(insertsql,
            apiName,
            waitingUsername,
            "",  // password
            "",  // authtoken
            0,   // usecount
            false, // tokenstatus
            false, // accountstatus
            0,   // usertobitid
            "",  // personid
            "free", // accounttype
            AccountStatus::WAITING  // status = waiting (待注册)
        );
        
        if(result.affectedRows() != 0)
        {
            // 获取插入的ID
            std::string getIdSql;
            if (dbType == DbType::SQLite3) {
                getIdSql = "SELECT last_insert_rowid() as id";
            } else if (dbType == DbType::MySQL) {
                getIdSql = "SELECT LAST_INSERT_ID() as id";
            } else {
                getIdSql = "SELECT currval(pg_get_serial_sequence('account','id')) as id";
            }
            auto idResult = dbClient->execSqlSync(getIdSql);
            if (idResult.size() > 0) {
                int waitingId = idResult[0]["id"].as<int>();
                LOG_INFO << "[账户数据库] 待注册账号创建成功, ID: " << waitingId;
                return waitingId;
            }
        }
    } catch(const std::exception& e) {
        LOG_ERROR << "[账户数据库] 创建待注册账号失败: " << e.what();
    }
    
    return -1;
}

bool AccountDbManager::activateAccount(int waitingId, struct Accountinfo_st accountinfo)
{
    LOG_INFO << "[账户数据库] 激活账号, ID: " << waitingId << ", 用户名: " << accountinfo.userName;
    
    // 支持从 waiting 或 registering 状态激活
    // createtime 可能为空；PG 不接受 '' -> TIMESTAMP。
    // 为空则不更新 createtime（保留默认/原值），否则写入指定值。
    std::string updatesqlWithCreateTime = "update account set username=$1,password=$2,authtoken=$3,usecount=$4,tokenstatus=$5,accountstatus=$6,usertobitid=$7,personid=$8,accounttype=$9,status=$10,createtime=$11 where id=$12 and (status=$13 or status=$14)";
    std::string updatesqlNoCreateTime   = "update account set username=$1,password=$2,authtoken=$3,usecount=$4,tokenstatus=$5,accountstatus=$6,usertobitid=$7,personid=$8,accounttype=$9,status=$10 where id=$11 and (status=$12 or status=$13)";

    try {
        auto result = [&]() {
            if (accountinfo.createTime.empty())
            {
                return dbClient->execSqlSync(
                    updatesqlNoCreateTime,
                    accountinfo.userName,
                    accountinfo.passwd,
                    accountinfo.authToken,
                    accountinfo.useCount,
                    accountinfo.tokenStatus,
                    accountinfo.accountStatus,
                    accountinfo.userTobitId,
                    accountinfo.personId,
                    accountinfo.accountType,
                    AccountStatus::ACTIVE,  // 激活为active状态
                    waitingId,
                    AccountStatus::WAITING,     // 从待注册状态激活
                    AccountStatus::REGISTERING  // 从注册中状态激活
                );
            }

            return dbClient->execSqlSync(
                updatesqlWithCreateTime,
                accountinfo.userName,
                accountinfo.passwd,
                accountinfo.authToken,
                accountinfo.useCount,
                accountinfo.tokenStatus,
                accountinfo.accountStatus,
                accountinfo.userTobitId,
                accountinfo.personId,
                accountinfo.accountType,
                AccountStatus::ACTIVE,  // 激活为active状态
                accountinfo.createTime,
                waitingId,
                AccountStatus::WAITING,     // 从待注册状态激活
                AccountStatus::REGISTERING  // 从注册中状态激活
            );
        }();
        
        if(result.affectedRows() != 0)
        {
            LOG_INFO << "[账户数据库] 账号激活成功: " << accountinfo.userName;
            return true;
        }
        else
        {
            LOG_ERROR << "[账户数据库] 账号激活失败: 未找到待注册/注册中记录或已被激活";
            return false;
        }
    } catch(const std::exception& e) {
        LOG_ERROR << "[账户数据库] 激活账号异常: " << e.what();
        return false;
    }
}

bool AccountDbManager::deleteWaitingAccount(int waitingId)
{
    LOG_INFO << "[账户数据库] 删除待注册账号, ID: " << waitingId;
    
    // 只能删除 waiting 状态的账号，不能删除 registering 状态的
    std::string deletesql = "delete from account where id=$1 and status=$2";
    
    try {
        auto result = dbClient->execSqlSync(deletesql, waitingId, AccountStatus::WAITING);
        
        if(result.affectedRows() != 0)
        {
            LOG_INFO << "[账户数据库] 待注册账号删除成功";
            return true;
        }
        else
        {
            LOG_WARN << "[账户数据库] 待注册账号删除失败: 未找到记录或状态不是待注册";
            return false;
        }
    } catch(const std::exception& e) {
        LOG_ERROR << "[账户数据库] 删除待注册账号异常: " << e.what();
        return false;
    }
}

int AccountDbManager::countAccountsByChannel(string apiName, bool includeWaiting)
{
    std::string countsql;
    if (includeWaiting) {
        countsql = "select count(*) as cnt from account where apiname=$1";
    } else {
        // 排除 waiting, registering, pending 状态
        countsql = "select count(*) as cnt from account where apiname=$1 and status=$2";
    }
    
    try {
        int count = 0;
        if (includeWaiting) {
            auto result = dbClient->execSqlSync(countsql, apiName);
            if (result.size() > 0) {
                count = result[0]["cnt"].as<int>();
            }
        } else {
            auto result = dbClient->execSqlSync(countsql, apiName, AccountStatus::ACTIVE);
            if (result.size() > 0) {
                count = result[0]["cnt"].as<int>();
            }
        }
        
        LOG_DEBUG << "[账户数据库] 渠道 " << apiName << " 账号数量: " << count << " (includeWaiting=" << includeWaiting << ")";
        return count;
    } catch(const std::exception& e) {
        LOG_ERROR << "[账户数据库] 统计账号数量异常: " << e.what();
        return 0;
    }
}

bool AccountDbManager::updateAccountStatus(string apiName, string userName, string status)
{
    LOG_INFO << "[账户数据库] 更新账号状态: " << apiName << "/" << userName << " -> " << status;
    
    std::string updatesql = "update account set status=$1 where apiname=$2 and username=$3";
    
    try {
        auto result = dbClient->execSqlSync(updatesql, status, apiName, userName);
        
        if(result.affectedRows() != 0)
        {
            LOG_INFO << "[账户数据库] 账号状态更新成功";
            return true;
        }
        else
        {
            LOG_ERROR << "[账户数据库] 账号状态更新失败: 未找到账号";
            return false;
        }
    } catch(const std::exception& e) {
        LOG_ERROR << "[账户数据库] 更新账号状态异常: " << e.what();
        return false;
    }
}

bool AccountDbManager::updateAccountStatusById(int id, string status)
{
    LOG_INFO << "[账户数据库] 根据ID更新账号状态: ID=" << id << " -> " << status;
    
    std::string updatesql = "update account set status=$1 where id=$2";
    
    try {
        auto result = dbClient->execSqlSync(updatesql, status, id);
        
        if(result.affectedRows() != 0)
        {
            LOG_INFO << "[账户数据库] 账号状态更新成功";
            return true;
        }
        else
        {
            LOG_ERROR << "[账户数据库] 账号状态更新失败: 未找到账号ID=" << id;
            return false;
        }
    } catch(const std::exception& e) {
        LOG_ERROR << "[账户数据库] 更新账号状态异常: " << e.what();
        return false;
    }
}

string AccountDbManager::getAccountStatusById(int id)
{
    std::string selectsql = "select status from account where id=$1";
    
    try {
        auto result = dbClient->execSqlSync(selectsql, id);
        if (result.size() > 0 && !result[0]["status"].isNull()) {
            return result[0]["status"].as<std::string>();
        }
    } catch(const std::exception& e) {
        LOG_ERROR << "[账户数据库] 获取账号状态异常: " << e.what();
    }
    
    return "";
}

string AccountDbManager::getAccountStatusByUsername(string apiName, string userName)
{
    std::string selectsql = "select status from account where apiname=$1 and username=$2";
    
    try {
        auto result = dbClient->execSqlSync(selectsql, apiName, userName);
        if (result.size() > 0 && !result[0]["status"].isNull()) {
            return result[0]["status"].as<std::string>();
        }
    } catch(const std::exception& e) {
        LOG_ERROR << "[账户数据库] 获取账号状态异常: " << e.what();
    }
    
    return "";
}