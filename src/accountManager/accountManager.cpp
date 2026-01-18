#include "accountManager.h"
#include<drogon/drogon.h>
#include <ApiManager.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/DbClient.h>
#include <cstdlib>
#include "../dbManager/channel/channelDbManager.h"
using namespace drogon;
using namespace drogon::orm;

// 从环境变量读取登录服务 URL，默认值为本地 127.0.0.1
string getLoginServiceUrl(const string& name) {
    // 1. 优先从配置文件读取
    auto customConfig = drogon::app().getCustomConfig();
    if (customConfig.isMember("login_service_urls") && customConfig["login_service_urls"].isArray()) {
        for (const auto& service : customConfig["login_service_urls"]) {
            if (service.isMember("name") && service["name"].asString() == name && service.isMember("url")) {
                string url = service["url"].asString();
                if (!url.empty()) {
                    return url; // 返回完整的 URL，不再拼接
                }
            }
        }
    }

    // 2. 其次从环境变量读取 (作为后备)
    const char* envUrl = std::getenv("LOGIN_SERVICE_URL");
    if (envUrl != nullptr && strlen(envUrl) > 0 && name == "chaynsapi") {
        return string(envUrl);
    }

    // 3. 最后使用默认值 (作为后备)
    if (name == "chaynsapi") {
        return "http://127.0.0.1:5557/aichat/chayns/login";
    }
    
    return ""; // 如果找不到，返回空字符串
}
// 从环境变量读取登录服务 URL，默认值为本地 127.0.0.1
string getRegistServiceUrl(const string& name) {
    // 1. 优先从配置文件读取
    auto customConfig = drogon::app().getCustomConfig();
    if (customConfig.isMember("regist_service_urls") && customConfig["regist_service_urls"].isArray()) {
        for (const auto& service : customConfig["regist_service_urls"]) {
            if (service.isMember("name") && service["name"].asString() == name && service.isMember("url")) {
                string url = service["url"].asString();
                if (!url.empty()) {
                    return url; // 返回完整的 URL，不再拼接
                }
            }
        }
    }

    // 2. 其次从环境变量读取 (作为后备)
    const char* envUrl = std::getenv("LOGIN_SERVICE_URL");
    if (envUrl != nullptr && strlen(envUrl) > 0 && name == "chaynsapi") {
        return string(envUrl);
    }

    // 3. 最后使用默认值 (作为后备)
    if (name == "chaynsapi") {
        return "http://127.0.0.1:5557/aichat/chayns/autoregister";
    }
    
    return ""; // 如果找不到，返回空字符串
}

AccountManager::AccountManager()
{

}
AccountManager::~AccountManager()
{
}
void AccountManager::init()
{
    LOG_INFO << "AccountManager::init start";
    accountDbManager = AccountDbManager::getInstance();
    if(!accountDbManager->isTableExist())
    {
        accountDbManager->createTable();
    }
    else
    {
        accountDbManager->checkAndUpgradeTable();
    }
    loadAccount();
    //checkUpdateAccountToken();
    checkUpdateTokenthread();
    waitUpdateAccountTokenThread();
    //checkAccountCountThread();  // 已改为事件驱动，不再定时检查
    checkAccountTypeThread();   // 已改为事件驱动，不再定时检查
    LOG_INFO << "AccountManager::init end";
}
 
void AccountManager::loadAccount()
{
    accountPoolMap.clear();
    LOG_INFO << "loadAccount start";
    //load account from config.json
    if(accountDbManager->isTableExist())
    {
        loadAccountFromDatebase();
    }
    else
    {
        loadAccountFromConfig();
    }

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
    auto configAccountList = customConfig["account"];

    //and add to accountPoolMap
    for(auto& account : configAccountList)
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
        auto createTime = account["createTime"].empty()?"":account["createTime"].asString();
        auto accountType = account["accountType"].empty()?"free":account["accountType"].asString();
        addAccount(apiName,userName,passwd,authToken,useCount,tokenStatus,accountStatus,userTobitId,personId,createTime,accountType);
    }
    LOG_INFO << "loadAccountFromConfig end";
}
void AccountManager::addAccount(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId,string personId,string createTime,string accountType)
{
    auto account = make_shared<Accountinfo_st>(apiName,userName,passwd,authToken,useCount,tokenStatus,accountStatus,userTobitId,personId,createTime,accountType);
    accountList[apiName][userName] = account;
    if(accountPoolMap[apiName] == nullptr)
    {
        accountPoolMap[apiName] = make_shared<priority_queue<shared_ptr<Accountinfo_st>,vector<shared_ptr<Accountinfo_st>>,AccountCompare>>();
    }
    accountPoolMap[apiName]->push(account);
}
bool AccountManager::addAccountbyPost(Accountinfo_st accountinfo)
{
    if(accountList[accountinfo.apiName].find(accountinfo.userName) != accountList[accountinfo.apiName].end())
    {
        return false;
    }
    addAccount(accountinfo.apiName,accountinfo.userName,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId,accountinfo.createTime,accountinfo.accountType);
    return true;
}
bool AccountManager::updateAccount(Accountinfo_st accountinfo)
{
    if(accountList.find(accountinfo.apiName) == accountList.end() || 
       accountList[accountinfo.apiName].find(accountinfo.userName) == accountList[accountinfo.apiName].end())
    {
        return false;
    }
    auto account = accountList[accountinfo.apiName][accountinfo.userName];
    account->passwd = accountinfo.passwd;
    account->authToken = accountinfo.authToken;
    account->useCount = accountinfo.useCount;
    account->tokenStatus = accountinfo.tokenStatus;
    account->accountStatus = accountinfo.accountStatus;
    account->userTobitId = accountinfo.userTobitId;
    account->personId = accountinfo.personId;
    account->accountType = accountinfo.accountType;
    return true;
}
bool AccountManager::deleteAccountbyPost(string apiName,string userName)
{

    if(accountList.find(apiName) != accountList.end() && accountList[apiName].find(userName) != accountList[apiName].end())
    {
        accountList[apiName][userName]->tokenStatus = false;
        accountList[apiName][userName]->accountStatus = false;
        accountList[apiName].erase(userName);
        return true;
    }
    return false;   
}
void AccountManager::getAccount(string apiName,shared_ptr<Accountinfo_st>& account, string accountType)
{
    if(accountPoolMap.find(apiName) == accountPoolMap.end() || accountPoolMap[apiName] == nullptr || accountPoolMap[apiName]->empty())
    {
        LOG_ERROR << "accountPoolMap[" << apiName << "] is empty or not found";
        return;
    }

    if (accountType.empty()) {
        account = accountPoolMap[apiName]->top();
        accountPoolMap[apiName]->pop();
        if(account->tokenStatus)
        {
            account->useCount++;
            LOG_INFO << "useCount incremented for " << account->userName << ", new value: " << account->useCount;
            LOG_INFO << "accountList incremented for " << accountList[apiName][account->userName]->userName << ", new value: " << accountList[apiName][account->userName]->useCount;

        }
        accountPoolMap[apiName]->push(account);
    } else {
        vector<shared_ptr<Accountinfo_st>> tempAccounts;
        bool found = false;
        
        while(!accountPoolMap[apiName]->empty()) {
            auto currentAccount = accountPoolMap[apiName]->top();
            accountPoolMap[apiName]->pop();
            
            if (currentAccount->accountType == accountType) {
                account = currentAccount;
                found = true;
                if(account->tokenStatus)
                {
                    account->useCount++;
                    LOG_INFO << "useCount incremented for " << account->userName << " (" << accountType << "), new value: " << account->useCount;
                }
                accountPoolMap[apiName]->push(account);
                break;
            }
            tempAccounts.push_back(currentAccount);
        }
        
        for(const auto& acc : tempAccounts) {
            accountPoolMap[apiName]->push(acc);
        }
        
        if (!found) {
            LOG_ERROR << "No account found with type: " << accountType << " for api: " << apiName;
        }
    }
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
    request->addHeader("Authorization","Bearer " + token);
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
    LOG_INFO << "getChaynsToken start";
    const string fullUrl = getLoginServiceUrl("chaynsapi");
    if (fullUrl.empty()) {
        LOG_ERROR << "login_service_url for 'chaynsapi' not found in config.";
        return Json::Value();
    }
    LOG_INFO << "fullurl： "<<fullUrl;

    // 解析 URL
    string baseUrl, path;
    size_t protocolPos = fullUrl.find("://");
    if (protocolPos == string::npos) {
        LOG_ERROR << "Invalid login service URL format: " << fullUrl;
        return Json::Value();
    }
    size_t pathPos = fullUrl.find('/', protocolPos + 3);
    if (pathPos == string::npos) {
        baseUrl = fullUrl;
        path = "/";
    } else {
        baseUrl = fullUrl.substr(0, pathPos);
        path = fullUrl.substr(pathPos);
    }
    LOG_INFO << "baseUrl： "<<baseUrl;

     // 等待服务器可用
    if (!isServerReachable(baseUrl)) {
        LOG_ERROR << "Server is not reachable after maximum retries: " << baseUrl;
        return Json::Value(); // 返回空的Json对象
    }
    auto client = HttpClient::newHttpClient(baseUrl);
    auto request = HttpRequest::newHttpRequest();
    Json::Value json;
    json["username"] = username;
    json["password"] = passwd;
    request->setMethod(HttpMethod::Post);
    request->setPath(path);
    request->setContentTypeString("application/json");
    request->setBody(json.toStyledString());
    auto [result, response] = client->sendRequest(request);
    Json::CharReaderBuilder reader;
    Json::Value responsejson;
    string body="";
    if(response->getStatusCode()==200)
    {
        body=std::string(response->getBody());
        LOG_INFO << "Login service response body: " << body;
    }
    string errs;
    istringstream s(body);
    if (!Json::parseFromStream(reader, s, &responsejson, &errs)) {
        LOG_ERROR << "Failed to parse login response JSON: " << errs;
    }
    return responsejson;
}
void AccountManager::checkToken()
{
    LOG_INFO << "checkToken start";
    LOG_INFO << "checkToken function map size: " << checkTokenMap.size();
    for(auto apiName:accountList)
    {   
        for(auto& userName:apiName.second)
        {
            LOG_INFO << "checkToken accountinfo: " << userName.second->apiName << " " << userName.second->userName;
            if(checkTokenMap[userName.second->apiName])
            {
                bool result = (this->*checkTokenMap[userName.second->apiName])(userName.second->authToken);
                LOG_INFO << "checkToken result: " << result;
                setStatusTokenStatus(userName.second->apiName,userName.second->userName,result);
                
            }
            else
            {
                LOG_ERROR << "apiName: " << userName.second->apiName << " is not supported";
            }
        }   
    }
    LOG_INFO << "checkToken end";
}

void AccountManager::updateToken()
{
    LOG_INFO << "updateToken start";
    for(auto apiName:accountList)
    {
        for(auto& userName:apiName.second)
        {
            if(!userName.second->tokenStatus||userName.second->authToken.empty())
            {
                if(updateTokenMap[userName.second->apiName])
           {
                (this->*updateTokenMap[userName.second->apiName])(userName.second);
                if(userName.second->tokenStatus)
                {
                    accountDbManager->updateAccount(*(userName.second.get()));
                    refreshAccountQueue(userName.second->apiName);
                }
           }
           else
                {
                    LOG_ERROR << "apiName: " << userName.second->apiName << " is not supported";
                }
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
                bool hasProAccess = false;
                if (token.isMember("has_pro_access") && token["has_pro_access"].asBool()) {
                    hasProAccess = true;
                }
                string accountType = hasProAccess ? "pro" : "free";
                accountinfo->accountType=accountType;
    }
    LOG_INFO << "updateChaynsToken end";
}
void AccountManager::checkUpdateTokenthread()
{
    thread t1([&]{
        while(true) 
        {
            checkToken();
            this_thread::sleep_for(chrono::hours(1));
            //updateToken();
        }
    });
    t1.detach();
}
void AccountManager::checkUpdateAccountToken()  
{
    checkToken();
   // updateToken();
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
    auto accountDBList = accountDbManager->getAccountDBList();
    for(auto& accountinfo:accountDBList)
    {
        LOG_INFO << "Loading account from DB: " << accountinfo.userName << ", personId: " << accountinfo.personId << ", createTime: " << accountinfo.createTime;
        addAccount(accountinfo.apiName,accountinfo.userName,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId,accountinfo.createTime,accountinfo.accountType);
    }
    LOG_INFO << "loadDatebase end size "<<accountDBList.size();
}
void AccountManager::saveAccountToDatebase()
{
    LOG_INFO << "saveAccountToDatebase start";
    for(auto& apiName:accountList)
    {
        for(auto& userName:apiName.second)
        {
            accountDbManager->addAccount(*(userName.second.get()));
        }
    }
    LOG_INFO << "saveAccountToDatebase end";
}

map<string,map<string,shared_ptr<Accountinfo_st>>> AccountManager::getAccountList()
{
    return accountList;
}
void AccountManager::setStatusAccountStatus(string apiName,string userName,bool status)
{
    if(accountList.find(apiName) != accountList.end() && accountList[apiName].find(userName) != accountList[apiName].end())
    {
        accountList[apiName][userName]->accountStatus = status;
    }
}
void AccountManager::setStatusTokenStatus(string apiName,string userName,bool status)
{
    if(accountList.find(apiName) != accountList.end() && accountList[apiName].find(userName) != accountList[apiName].end())
    {
        accountList[apiName][userName]->tokenStatus = status;
        if(!status)
            {
                std::lock_guard<std::mutex> lock(accountListNeedUpdateMutex);
                accountListNeedUpdate.push_back(accountList[apiName][userName]);
                accountListNeedUpdateCondition.notify_one();
            }
    }
}
void AccountManager::waitUpdateAccountToken()
{
    LOG_INFO << "waitUpdateAccountTokenThread start";
    while (true) {  // 持续运行的工作循环
        shared_ptr<Accountinfo_st> account;
        
        // 获取待更新账号，仅在访问共享队列时加锁
        {
            std::unique_lock<std::mutex> lock(accountListNeedUpdateMutex);
            while (accountListNeedUpdate.empty()) {
                accountListNeedUpdateCondition.wait(lock);
            }
            
            account = accountListNeedUpdate.front();
            accountListNeedUpdate.pop_front();
            LOG_INFO << "Processing account update for user: " << account->userName;
        }

        // 验证账号
        if (!account) {
            LOG_ERROR << "Invalid account pointer";
            continue;
        }

        // 查找更新函数
        auto updateFunc = updateTokenMap.find(account->apiName);
        if (updateFunc == updateTokenMap.end()) {
            LOG_ERROR << "Unsupported API name: " << account->apiName;
            continue;
        }

        try {
            // 执行token更新
            (this->*(updateFunc->second))(account);

            // 仅在更新成功时更新数据库和刷新队列
            if (account->tokenStatus) {
                if (!accountDbManager->updateAccount(*account)) {
                    LOG_ERROR << "Failed to update account in database";
                    continue;
                }
                refreshAccountQueue(account->apiName);
                
                // Token 更新成功后，检查并更新 accountType
                LOG_INFO << "Token updated, checking accountType for " << account->userName;
                //updateAccountType(account);
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR << "Exception during account update: " << e.what() 
                     << " for user: " << account->userName;
        }
    };
}
void AccountManager::waitUpdateAccountTokenThread()
{
    std::thread worker(&AccountManager::waitUpdateAccountToken, this);
    worker.detach();
}

void AccountManager::checkAccountCountThread()
{
    std::thread t([this](){
        while(true)
        {
            LOG_INFO << "Checking channel account counts...";
            checkChannelAccountCounts();
            // Check every 10 minutes
            std::this_thread::sleep_for(std::chrono::minutes(10));
        }
    });
    t.detach();
}

void AccountManager::checkChannelAccountCounts()
{
    auto channelDbManager = ChannelDbManager::getInstance();
    auto channelList = channelDbManager->getChannelList();

    for(const auto& channel : channelList)
    {
        if(channel.accountCount > 0)
        {
            int currentCount = 0;
            // Count accounts for this channel (apiName)
            if(accountList.find(channel.channelName) != accountList.end())
            {
                currentCount = accountList[channel.channelName].size();
            }

            LOG_INFO << "Channel: " << channel.channelName << ", Target: " << channel.accountCount << ", Current: " << currentCount;

            if(currentCount < channel.accountCount)
            {
                int needed = channel.accountCount - currentCount;
                LOG_INFO << "Need to register " << needed << " accounts for " << channel.channelName;
                // Register one by one to avoid overwhelming the service
                for(int i=0; i<needed; ++i)
                {
                    autoRegisterAccount(channel.channelName);
                    // Small delay between registrations
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                }
            }
        }
    }
}

std::string generateRandomString(int length) {
    const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string ret;
    for (int i = 0; i < length; ++i) {
        ret += chars[rand() % chars.length()];
    }
    return ret;
}

void AccountManager::autoRegisterAccount(string apiName)
{
    LOG_INFO << "Starting auto-registration for channel: " << apiName;
    
    string firstName = "User" + generateRandomString(5);
    string lastName = "Auto" + generateRandomString(5);
    string password = "Pwd" + generateRandomString(8) + "!";

    Json::Value requestBody;
    requestBody["first_name"] = firstName;
    requestBody["last_name"] = lastName;
    requestBody["password"] = password;

    const string fullUrl = getRegistServiceUrl("chaynsapi");
    if (fullUrl.empty()) {
        LOG_ERROR << "login_service_url for 'chaynsapi' not found.";
        return;
    }

    // 解析 URL
    string baseUrl, path;
    size_t protocolPos = fullUrl.find("://");
    if (protocolPos == string::npos) {
        LOG_ERROR << "Invalid login service URL format: " << fullUrl;
        return ;
    }
    size_t pathPos = fullUrl.find('/', protocolPos + 3);
    if (pathPos == string::npos) {
        baseUrl = fullUrl;
        path = "/";
    } else {
        baseUrl = fullUrl.substr(0, pathPos);
        path = fullUrl.substr(pathPos);
    }
    LOG_INFO << "baseUrl： "<<baseUrl;

    auto client = HttpClient::newHttpClient(baseUrl);
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(HttpMethod::Post);
    request->setPath(path);
    request->setContentTypeString("application/json");
    request->setBody(requestBody.toStyledString());
    
    LOG_INFO << "Sending auto-register request...";
    auto [result, response] = client->sendRequest(request, 300.0);

    if (result != ReqResult::Ok || response->getStatusCode() != 200) {
        LOG_ERROR << "Auto-registration failed. Result: " << (int)result
                  << ", Status: " << (response ? response->getStatusCode() : 0)
                  << ", Body: " << (response ? response->getBody() : "");
        return;
    }

    Json::Value jsonResponse;
    Json::CharReaderBuilder reader;
    string errs;
    std::string responseBody(response->getBody());
    std::istringstream s(responseBody);
    
    if (!Json::parseFromStream(reader, s, &jsonResponse, &errs)) {
        LOG_ERROR << "Failed to parse auto-register response: " << errs;
        return;
    }

    string email = jsonResponse["email"].asString();
    string respPassword = jsonResponse["password"].asString();
    int userid = jsonResponse["userid"].asInt();
    string personid = jsonResponse["personid"].asString();
    string token = jsonResponse["token"].asString();
    bool hasProAccess = false;
    if (jsonResponse.isMember("has_pro_access") && jsonResponse["has_pro_access"].asBool()) {
        hasProAccess = true;
    }

    LOG_INFO << "Auto-registration successful for " << email;
    
    string createTime = trantor::Date::now().toDbStringLocal();
    string accountType = hasProAccess ? "pro" : "free";

    Accountinfo_st newAccount(apiName, email, respPassword, token, 0, true, true, userid, personid, createTime, accountType);
    
    if (accountDbManager->addAccount(newAccount)) {
        LOG_INFO << "New account added to database.";
        addAccount(apiName, email, respPassword, token, 0, true, true, userid, personid, createTime, accountType);
        
        // 自动注册账号后，检查并更新该账号的 accountType
        if (accountList.find(apiName) != accountList.end() &&
            accountList[apiName].find(email) != accountList[apiName].end()) {
            auto newAccountPtr = accountList[apiName][email];
            LOG_INFO << "Auto-registered account, checking accountType for " << email;
            updateAccountType(newAccountPtr);
        }
    } else {
        LOG_ERROR << "Failed to add new account to database.";
    }
}

// 获取用户 Pro 权限状态
// 参考 Python autoregister.py 中的 get_user_pro_access 方法
bool AccountManager::getUserProAccess(const string& token, const string& personId)
{
    LOG_INFO << "getUserProAccess start for personId: " << personId;
    
    // API URL: https://cube.tobit.cloud/ai-proxy/v1/userSettings/personId/{personId}
    string baseUrl = "https://cube.tobit.cloud";
    string path = "/ai-proxy/v1/userSettings/personId/" + personId;
    
    try {
        auto client = HttpClient::newHttpClient(baseUrl);
        auto request = HttpRequest::newHttpRequest();
        request->setMethod(HttpMethod::Get);
        request->setPath(path);
        request->addHeader("Content-Type", "application/json");
        request->addHeader("Authorization", "Bearer " + token);
        
        auto [result, response] = client->sendRequest(request, 30.0);
        
        if (result != ReqResult::Ok || !response) {
            LOG_ERROR << "getUserProAccess request failed";
            return false;
        }
        
        LOG_INFO << "getUserProAccess response status: " << response->getStatusCode();
        
        if (response->getStatusCode() == 200) {
            Json::CharReaderBuilder reader;
            Json::Value jsonResponse;
            string errs;
            string body = string(response->getBody());
            istringstream s(body);
            
            if (Json::parseFromStream(reader, s, &jsonResponse, &errs)) {
                if (jsonResponse.isMember("hasProAccess")) {
                    bool hasProAccess = jsonResponse["hasProAccess"].asBool();
                    LOG_INFO << "getUserProAccess result: hasProAccess=" << hasProAccess;
                    return hasProAccess;
                }
            } else {
                LOG_ERROR << "getUserProAccess JSON parse error: " << errs;
            }
        } else {
            LOG_ERROR << "getUserProAccess API error: " << response->getStatusCode()
                      << " - " << response->getBody();
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "getUserProAccess exception: " << e.what();
    }
    
    return false;
}

// 更新单个账号的 accountType
void AccountManager::updateAccountType(shared_ptr<Accountinfo_st> account)
{
    if (!account || account->authToken.empty() || account->personId.empty()) {
        LOG_ERROR << "updateAccountType: invalid account data";
        return;
    }
    
    LOG_INFO << "updateAccountType for user: " << account->userName;
    
    bool hasProAccess = getUserProAccess(account->authToken, account->personId);
    string newAccountType = hasProAccess ? "pro" : "free";
    
    if (account->accountType != newAccountType) {
        LOG_INFO << "Account type changed for " << account->userName
                 << ": " << account->accountType << " -> " << newAccountType;
        account->accountType = newAccountType;
        
        // 更新数据库
        if (accountDbManager->updateAccount(*account)) {
            LOG_INFO << "Account type updated in database for " << account->userName;
        } else {
            LOG_ERROR << "Failed to update account type in database for " << account->userName;
        }
    } else {
        LOG_INFO << "Account type unchanged for " << account->userName << ": " << account->accountType;
    }
}

// 更新所有账号的 accountType
void AccountManager::updateAllAccountTypes()
{
    LOG_INFO << "updateAllAccountTypes start";
    
    for (auto& apiName : accountList) {
        for (auto& userName : apiName.second) {
            auto account = userName.second;
            
            // 只更新 token 有效的账号
            if (account && account->tokenStatus && !account->authToken.empty()) {
                updateAccountType(account);
                
                // 添加短暂延迟，避免请求过于频繁
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
    
    LOG_INFO << "updateAllAccountTypes end";
}

// 启动定时检查 accountType 的线程
void AccountManager::checkAccountTypeThread()
{
    std::thread t([this]() {
        // 启动后等待 5 分钟再执行第一次检查，让系统稳定
        std::this_thread::sleep_for(std::chrono::minutes(2));
        
        while (true) {
            LOG_INFO << "Starting scheduled account type check...";
            updateAllAccountTypes();
            
            // 每 24 小时检查一次
            std::this_thread::sleep_for(std::chrono::hours(24));
        }
    });
    t.detach();
    LOG_INFO << "checkAccountTypeThread started";
}

