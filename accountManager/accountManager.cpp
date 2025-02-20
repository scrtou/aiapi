#include "accountManager.h"
#include<drogon/drogon.h>
#include <ApiManager.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/DbClient.h>
using namespace drogon;
using namespace drogon::orm;
AccountManager::AccountManager()
{

}
AccountManager::~AccountManager()
{
}
void AccountManager::init()
{
    createTable("account");
    loadAccount();
    checkUpdateTokenthread();
}   
 
void AccountManager::loadAccount()
{
    LOG_INFO << "loadAccount start";
    //load account from config.json
    if(isTableExist("account"))
    {
        loadAccountFromDatebase();
    }
    else
    {
        loadAccountFromConfig();
    }
    LOG_INFO << "accountList size: " << accountList.size();
    LOG_INFO << "accountPoolMap size: " << accountPoolMap.size();
    for(auto& apiName : accountPoolMap)
    {
        LOG_INFO << "apiName: " << apiName.first << ",accountQueue size: " << apiName.second->size();

    }
    LOG_INFO << "loadAccount end";
    //printAccountPoolMap();
}
void AccountManager::loadAccountFromConfig()
{
    LOG_INFO << "loadAccountFromConfig start";
     auto customConfig = app().getCustomConfig();
    auto accountList = customConfig["account"];

    //and add to accountPoolMap
    for(auto& account : accountList)
    {
        auto apiName =account["apiname"].empty()?"":account["apiname"].asString();
        auto userName = account["username"].empty()?"":account["username"].asString();
        auto passwd = account["passwd"].empty()?"":account["passwd"].asString();
        auto authToken =account["authToken"].empty()?"":account["authToken"].asString();
        auto useCount = account["usecount"].empty()?0:account["usecount"].asInt();
        auto tokenStatus = account["tokenStatus"].empty()?false:account["tokenStatus"].asBool();
        auto accountStatus = account["accountStatus"].empty()?false:account["accountStatus"].asBool();
        auto userTobitId = account["usertobitid"].empty()?0:account["usertobitid"].asInt();
        auto personId = account["personId"].empty()?"":account["personId"].asString();
        addAccount(apiName,userName,passwd,authToken,useCount,tokenStatus,accountStatus,userTobitId,personId);
    }
    LOG_INFO << "loadAccountFromConfig end";
}
void AccountManager::addAccount(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId,string personId)
{
    auto account = make_shared<Accountinfo_st>(apiName,userName,passwd,authToken,useCount,tokenStatus,accountStatus,userTobitId,personId);
    accountList.push_back(account);
    if(accountPoolMap[apiName] == nullptr)
    {
        accountPoolMap[apiName] = make_shared<priority_queue<shared_ptr<Accountinfo_st>,vector<shared_ptr<Accountinfo_st>>,AccountCompare>>();
    }
    accountPoolMap[apiName]->push(account);
}
void AccountManager::getAccount(string apiName,shared_ptr<Accountinfo_st>& account)
{
    if(accountPoolMap[apiName]->empty())
    {
        LOG_ERROR << "accountPoolMap[" << apiName << "] is empty";
        return;
    }
    account = accountPoolMap[apiName]->top();
    accountPoolMap[apiName]->pop();
    if(account->tokenStatus)
    {
        account->useCount++;
    }
    accountPoolMap[apiName]->push(account);
}
void AccountManager::checkAccount()
{
    LOG_INFO << "checkAccount start";
    //check account from accountList
    //and add to accountPoolMap
    LOG_INFO << "checkAccount end";
}   
void AccountManager::refreshAccountQueue(string apiName)
{
    shared_ptr<Accountinfo_st> account = accountPoolMap[apiName]->top();
    accountPoolMap[apiName]->pop();
    accountPoolMap[apiName]->push(account);
}
void AccountManager::printAccountPoolMap()
{
    LOG_INFO << "printAccountPoolMap start";
    for(auto& apiName : accountPoolMap)
    {
        LOG_INFO << "apiName: " << apiName.first << ",accountQueue size: " << apiName.second->size();
        auto tempQueue = *apiName.second;
       while(!tempQueue.empty())
       {
            auto account = tempQueue.top();
            LOG_INFO << "userName: " << account->userName;
            LOG_INFO << "passwd: " << account->passwd;
            LOG_INFO << "tokenStatus: " << account->tokenStatus;
            LOG_INFO << "accountStatus: " << account->accountStatus;
            LOG_INFO << "userTobitId: " << account->userTobitId;
            LOG_INFO << "useCount: " << account->useCount;
            LOG_INFO << "authToken: " << account->authToken;
            LOG_INFO << "--------------------------------"; 
            tempQueue.pop();
       }
    }
    LOG_INFO << "printAccountPoolMap end";
}


bool AccountManager::checkChaynsToken(string token)
{
    LOG_INFO << "checkChaynsTonen start";
    auto client = HttpClient::newHttpClient("https://webapi.tobit.com/AccountService/v1.0/Chayns/User");
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(HttpMethod::Get);
    request->setPath("/AccountService/v1.0/Chayns/User");
    request->addHeader("Authorization", token);
    auto [result, response] = client->sendRequest(request);
    LOG_DEBUG << "checkAlivableToken response: " << response->getStatusCode();
    LOG_INFO << "checkChaynsTonen end";
    if(response->getStatusCode()!=200)
    {
        return false;
    }
    return true;
}
Json::Value AccountManager::getChaynsToken(string username,string passwd)
{
    //LOG_INFO << "getChaynsToken start";
    const string serverUrl = "http://127.0.0.1:5556";
     // 等待服务器可用
    if (!isServerReachable(serverUrl)) {
        LOG_ERROR << "Server is not reachable after maximum retries";
        return Json::Value(); // 返回空的Json对象
    }
    auto client = HttpClient::newHttpClient(serverUrl);
    auto request = HttpRequest::newHttpRequest();
    Json::Value json;   
    json["username"] = username;
    json["password"] = passwd;
    request->setMethod(HttpMethod::Post);
    request->setPath("/aichat/chayns/login");
    request->setContentTypeString("application/json");
    request->setBody(json.toStyledString());
    auto [result, response] = client->sendRequest(request);
    Json::CharReaderBuilder reader;
    Json::Value responsejson;
    string body="";
    if(response->getStatusCode()==200)
    {
        body=std::string(response->getBody());
    }
    string errs;
    istringstream s(body);
    Json::parseFromStream(reader, s, &responsejson, &errs);
    return responsejson;
}
void AccountManager::checkToken()
{
    LOG_INFO << "checkToken start";
    LOG_INFO << "checkToken function map size: " << checkTokenMap.size();
    for(auto accountinfo:accountList)
    {   
        LOG_INFO << "checkToken accountinfo: " << accountinfo->apiName << " " << accountinfo->userName;
        if(checkTokenMap[accountinfo->apiName])
        {
            bool result = (this->*checkTokenMap[accountinfo->apiName])(accountinfo->authToken);
            accountinfo->tokenStatus = result;
            LOG_INFO << "checkToken result: " << result;
        }
        else
        {
            LOG_ERROR << "apiName: " << accountinfo->apiName << " is not supported";
        }
    }
    LOG_INFO << "checkToken end";
}

void AccountManager::updateToken()
{
    LOG_INFO << "updateToken start";
    for(auto accountinfo:accountList)
    {
        if(!accountinfo->tokenStatus||accountinfo->authToken.empty())
        {
           if(updateTokenMap[accountinfo->apiName])
           {
                (this->*updateTokenMap[accountinfo->apiName])(accountinfo);
                updateAccount(*(accountinfo.get()));
           }
           else
           {
            LOG_ERROR << "apiName: " << accountinfo->apiName << " is not supported";
           }
        }
    }
    LOG_INFO << "updateToken end";

}

void AccountManager::updateChaynsToken(shared_ptr<Accountinfo_st> accountinfo)
{
    LOG_INFO << "updateChaynsToken start " << accountinfo->userName;
    auto token = getChaynsToken(accountinfo->userName,accountinfo->passwd);
    LOG_INFO << "updateChaynsToken token result: " << (token.empty()?"empty":"not empty");
    if(!token.empty())
    {
                accountinfo->tokenStatus = true;
                accountinfo->authToken = token["token"].asString();
                accountinfo->accountStatus = true;
                accountinfo->useCount = 0;
                accountinfo->userTobitId = token["userid"].asInt();
                accountinfo->personId = token["personid"].asString();
    }
    LOG_INFO << "updateChaynsToken end";
}
void AccountManager::checkUpdateTokenthread()
{
    
        while(true) 
        {
            checkToken();
            updateToken();
            this_thread::sleep_for(chrono::hours(1));
        }
}
void AccountManager::checkUpdateAccountToken()  
{
    checkToken();
    updateToken();
}  

// 添加检测服务可用性的函数
bool AccountManager::isServerReachable(const string& host, int maxRetries ) {
    auto checkClient = HttpClient::newHttpClient(host);
    auto checkRequest = HttpRequest::newHttpRequest();
    checkRequest->setMethod(HttpMethod::Get);
    checkRequest->setPath("/health"); // 假设有健康检查端点，如果没有可以用 "/"

    int retryCount = 0;
    while (retryCount < maxRetries) {
        try {
            auto [checkResult, checkResponse] = checkClient->sendRequest(checkRequest);
            if (checkResponse && checkResponse->getStatusCode() == 200) {
                LOG_INFO << "Server is reachable after " << retryCount << " attempts";
                return true;
            }
        } catch (...) {
            LOG_INFO << "Server not reachable, retry attempt: " << retryCount + 1;
        }
        
        retryCount++;
        std::this_thread::sleep_for(std::chrono::seconds(1)); // 等待1秒后重试
    }
    
    return false;
}
void AccountManager::loadAccountFromDatebase()
{
    LOG_INFO << "loadAccountFromDatebase start";
    auto accountDBList = getAccountDBList();
    for(auto& accountinfo:accountDBList)
    {
        addAccount(accountinfo.apiName,accountinfo.userName,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId);
    }
    LOG_INFO << "loadDatebase end";
}
void AccountManager::saveAccountToDatebase()
{
    LOG_INFO << "saveAccountToDatebase start";
    for(auto& accountinfo:accountList)
    {
        updateAccount(*(accountinfo.get()));
    }
    LOG_INFO << "saveAccountToDatebase end";
}

bool AccountManager::addAccount(struct Accountinfo_st accountinfo)
{
    auto dbClient = app().getDbClient("aichatpg");
    std::string selectsql = "select * from account where apiname=$1 and username=$2";
    std::string insertsql = "insert into account (apiname,username,password,authtoken,usecount,tokenstatus,accountstatus,usertobitid,personid) values ($1,$2,$3,$4,$5,$6,$7,$8,$9)";
    std::string updatesql = "update account set password=$1,authtoken=$2,usecount=$3,tokenstatus=$4,accountstatus=$5,usertobitid=$6,personid=$7 where apiname=$8 and username=$9";
    auto result = dbClient->execSqlSync(selectsql,accountinfo.apiName,accountinfo.userName);
    if(result.size()!=0)
    {
        LOG_INFO << "账号 " << accountinfo.userName << " 已存在";
        return false;
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
bool AccountManager::updateAccount(struct Accountinfo_st accountinfo)
{
    auto dbClient = app().getDbClient("aichatpg");
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
bool AccountManager::deleteAccount(struct Accountinfo_st accountinfo)
{
    auto dbClient = app().getDbClient("aichatpg");
    std::string deletesql = "delete from account where apiname=$1 and username=$2";
    auto result = dbClient->execSqlSync(deletesql,accountinfo.apiName,accountinfo.userName);
    if(result.affectedRows()!=0)
    {
        LOG_INFO << "账号 " << accountinfo.userName << " 删除成功";
        return true;
    }
    else
    {
        LOG_ERROR << "账号 " << accountinfo.userName << " 删除失败";
        return false;
    }
}
list<shared_ptr<Accountinfo_st>> AccountManager::getAccountList()
{
    return accountList;
}
list<Accountinfo_st> AccountManager::getAccountDBList()
{
    std::string selectsql = "select apiname,username,password,authtoken,usecount,tokenstatus,accountstatus,usertobitid,personid from account";
    auto dbClient = app().getDbClient("aichatpg");
    auto result = dbClient->execSqlSync(selectsql);
    list<Accountinfo_st> accountDBList;
    for(auto& item:result)
    {
        Accountinfo_st accountinfo(item["apiname"].as<std::string>(),item["username"].as<std::string>(),item["password"].as<std::string>(),item["authtoken"].as<std::string>(),item["usecount"].as<int>(),item["tokenstatus"].as<bool>(),item["accountstatus"].as<bool>(),item["usertobitid"].as<int>(),item["personid"].as<std::string>());
        accountDBList.push_back(accountinfo);
    }
    return accountDBList;
}
bool AccountManager::isTableExist(string tableName)
{
    auto dbClient = app().getDbClient("aichatpg");
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
void AccountManager::createTable(string tableName)
{
    auto dbClient = app().getDbClient("aichatpg");
    std::string sql = "create table if not exists " + tableName + " (id int auto_increment primary key,updatetime datetime default current_timestamp,apiname varchar(255),username varchar(255),password varchar(255),authtoken text,usecount int,tokenstatus boolean,accountstatus boolean,usertobitid int,personid varchar(255))";
    try 
    {
        dbClient->execSqlSync(sql);
    }
    catch(const std::exception& e)
    {
        LOG_ERROR << "createTable error: " << e.what();
    }
    
}