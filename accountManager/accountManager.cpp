#include "accountManager.h"
#include<drogon/drogon.h>
#include <ApiManager.h>
using namespace drogon;
AccountManager::AccountManager()
{

}
AccountManager::~AccountManager()
{
}
void AccountManager::init()
{
    loadAccount();
}   
 
void AccountManager::loadAccount()
{
    LOG_INFO << "loadAccount start";
    //load account from config.json
    auto customConfig = app().getCustomConfig();
    auto accountList = customConfig["account"];

    //and add to accountPoolMap
    for(auto& account : accountList)
    {
        auto apiName = account["apiname"].asString();
        auto userName = account["username"].asString();
        auto passwd = account["passwd"].asString();
        auto authToken = account["authToken"].asString();
        auto useCount = 0;
        auto tokenStatus = true;
        auto accountStatus = true;
        auto userTobitId = account["userTobitId"].asInt();
        addAccount(apiName,userName,passwd,authToken,useCount,tokenStatus,accountStatus,userTobitId);
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
void AccountManager::addAccount(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId)
{
    auto account = make_shared<Accountinfo_st>(apiName,userName,passwd,authToken,useCount,tokenStatus,accountStatus,userTobitId);
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

void AccountManager::checkToken()
{
    LOG_INFO << "checkToken start";
    for(auto accountinfo:accountList)
    {
        auto apiInterface = ApiManager::getInstance().getApiByApiName(accountinfo->apiName);
        if(apiInterface != nullptr)
        {
            bool tokenStatus = apiInterface->checkAlivableToken(accountinfo->authToken);
            accountinfo->tokenStatus = tokenStatus; 
            LOG_INFO << "apiName: " << accountinfo->apiName << ",userName: " << accountinfo->userName << ",tokenStatus: " << accountinfo->tokenStatus;   
        }
        else
        {
            LOG_ERROR << "apiInterface:[" << accountinfo->apiName << "] is nullptr";
        }
      
    }
    LOG_INFO << "checkToken end";
}