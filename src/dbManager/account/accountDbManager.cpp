#include "accountDbManager.h"
//pg create table 
std::string tableName = "account";
std::string createTablePgSql = R"(
    CREATE TABLE IF NOT EXISTS account (
        id SERIAL PRIMARY KEY,
        updatetime TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        apiname VARCHAR(255),
        username VARCHAR(255),
        password VARCHAR(255),
        authtoken TEXT,
        usecount INTEGER,
        tokenstatus BOOLEAN,
        accountstatus BOOLEAN,
        usertobitid INTEGER,
        personid VARCHAR(255)
    );
)";
std::string createTableSqlMysql=R"(
    CREATE TABLE IF NOT EXISTS account ( 
    id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, 
    updatetime DATETIME DEFAULT CURRENT_TIMESTAMP,
    apiname VARCHAR(255),
    username VARCHAR(255),
    password VARCHAR(255),
    authtoken TEXT,
    usecount INT,
    tokenstatus TINYINT(1),
    accountstatus TINYINT(1),
    usertobitid INT,
    personid VARCHAR(255)
) ENGINE=InnoDB;)";


void AccountDbManager::init()
{
    LOG_INFO << "AccountDbManager::init start";
    dbClient = app().getDbClient("aichatpg");
    LOG_INFO << "AccountDbManager::init end";
}

bool AccountDbManager::addAccount(struct Accountinfo_st accountinfo)
{
    std::string selectsql = "select * from account where apiname=$1 and username=$2";
    std::string insertsql = "insert into account (apiname,username,password,authtoken,usecount,tokenstatus,accountstatus,usertobitid,personid) values ($1,$2,$3,$4,$5,$6,$7,$8,$9)";
    std::string updatesql = "update account set password=$1,authtoken=$2,usecount=$3,tokenstatus=$4,accountstatus=$5,usertobitid=$6,personid=$7 where apiname=$8 and username=$9";
    auto result = dbClient->execSqlSync(selectsql,accountinfo.apiName,accountinfo.userName);
    if(result.size()!=0)
    {
        LOG_INFO << "账号 " << accountinfo.userName << " 已存在,更新账号";
        return updateAccount(accountinfo);
    }
    else
    {
         auto result1 =dbClient->execSqlSync(insertsql,accountinfo.apiName,accountinfo.userName,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId);
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
    std::string updatesql = "update account set password=$1,authtoken=$2,usecount=$3,tokenstatus=$4,accountstatus=$5,usertobitid=$6,personid=$7 where apiname=$8 and username=$9";
    auto result = dbClient->execSqlSync(updatesql,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId,accountinfo.apiName,accountinfo.userName);
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
    std::string selectsql = "select apiname,username,password,authtoken,usecount,tokenstatus,accountstatus,usertobitid,personid from account";
    auto result = dbClient->execSqlSync(selectsql);
    list<Accountinfo_st> accountDBList;
    for(auto& item:result)
    {
        Accountinfo_st accountinfo(item["apiname"].as<std::string>(),item["username"].as<std::string>(),item["password"].as<std::string>(),item["authtoken"].as<std::string>(),item["usecount"].as<int>(),item["tokenstatus"].as<bool>(),item["accountstatus"].as<bool>(),item["usertobitid"].as<int>(),item["personid"].as<std::string>());
        accountDBList.push_back(accountinfo);
    }
    return accountDBList;
}
bool AccountDbManager::isTableExist()
{
    auto result = dbClient->execSqlSync("select * from information_schema.tables where table_name='" + tableName + "'");
    if(result.size()!=0)
    {
        return true;
    }
    else
    {
        return false;
    }
}
void AccountDbManager::createTable()
{
    try 
    {
        dbClient->execSqlSync(createTablePgSql);
    }
    catch(const std::exception& e)
    {
        LOG_ERROR << "createTable error: " << e.what();
    }
    
}