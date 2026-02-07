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
    LOG_INFO << "[账户管理] 初始化开始";
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
    LOG_INFO << "[账户管理] 配置信息:";
    const string fullUrl = getLoginServiceUrl("chaynsapi");
    const string fullUrl1 = getRegistServiceUrl("chaynsapi");
    LOG_INFO << "[账户管理] 登录服务URL: " << fullUrl;
    LOG_INFO << "[账户管理] 注册服务URL: " << fullUrl1;
    LOG_INFO << "[账户管理] 初始化完成";
}
 
void AccountManager::loadAccount()
{
    accountPoolMap.clear();
    LOG_INFO << "[账户管理] 加载账户开始";
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
        LOG_INFO << "[账户管理] API名称: " << apiName.first << ", 账户队列大小: " << apiName.second->size();

    }
    LOG_INFO << "[账户管理] 加载账户完成";
    //printAccountPoolMap();
}
void AccountManager::loadAccountFromConfig()
{
    LOG_INFO << "[账户管理] 从配置文件加载账户开始";
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
    LOG_INFO << "[账户管理] 从配置文件加载账户完成";
}
void AccountManager::addAccount(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId,string personId,string createTime,string accountType,string status)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    auto account = make_shared<Accountinfo_st>(apiName,userName,passwd,authToken,useCount,tokenStatus,accountStatus,userTobitId,personId,createTime,accountType,status);
    accountList[apiName][userName] = account;
    
    // 只有 active 状态的账号才加入账号池
    if (status == AccountStatus::ACTIVE) {
        if(accountPoolMap[apiName] == nullptr)
        {
            accountPoolMap[apiName] = make_shared<priority_queue<shared_ptr<Accountinfo_st>,vector<shared_ptr<Accountinfo_st>>,AccountCompare>>();
        }
        accountPoolMap[apiName]->push(account);
    } else {
        LOG_INFO << "[账户管理] 账号 " << userName << " 状态为 " << status << ", 不加入账号池";
    }
}
bool AccountManager::addAccountbyPost(Accountinfo_st accountinfo)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    if(accountList[accountinfo.apiName].find(accountinfo.userName) != accountList[accountinfo.apiName].end())
    {
        return false;
    }
    // 注意：这里不能调用 addAccount，因为它也会获取锁，会导致死锁
    auto account = make_shared<Accountinfo_st>(accountinfo.apiName,accountinfo.userName,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId,accountinfo.createTime,accountinfo.accountType,accountinfo.status);
    accountList[accountinfo.apiName][accountinfo.userName] = account;
    
    // 只有 active 状态的账号才加入账号池
    if (accountinfo.status == AccountStatus::ACTIVE) {
        if(accountPoolMap[accountinfo.apiName] == nullptr)
        {
            accountPoolMap[accountinfo.apiName] = make_shared<priority_queue<shared_ptr<Accountinfo_st>,vector<shared_ptr<Accountinfo_st>>,AccountCompare>>();
        }
        accountPoolMap[accountinfo.apiName]->push(account);
    } else {
        LOG_INFO << "[账户管理] 账号 " << accountinfo.userName << " 状态为 " << accountinfo.status << ", 不加入账号池";
    }
    return true;
}
bool AccountManager::updateAccount(Accountinfo_st accountinfo)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
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
    account->status = accountinfo.status;
    return true;
}
bool AccountManager::deleteAccountbyPost(string apiName,string userName)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
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
    std::lock_guard<std::mutex> lock(accountListMutex);
    if(accountPoolMap.find(apiName) == accountPoolMap.end() || accountPoolMap[apiName] == nullptr || accountPoolMap[apiName]->empty())
    {
        LOG_ERROR << "[账户管理] 账户池 [" << apiName << "] 为空或未找到";
        return;
    }

    if (accountType.empty()) {
        account = accountPoolMap[apiName]->top();
        accountPoolMap[apiName]->pop();
        if(account->tokenStatus)
        {
            account->useCount++;
            LOG_INFO << "[账户管理] 使用次数已增加: " << account->userName << ", 新值: " << account->useCount;
            if (accountList.find(apiName) != accountList.end() &&
                accountList[apiName].find(account->userName) != accountList[apiName].end()) {
                LOG_INFO << "[账户管理] 账户列表已更新: " << accountList[apiName][account->userName]->userName << ", 新值: " << accountList[apiName][account->userName]->useCount;
            }
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
                    LOG_INFO << "[账户管理] 使用次数已增加: " << account->userName << " (" << accountType << "), 新值: " << account->useCount;
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
            LOG_ERROR << "[账户管理] 未找到类型为 " << accountType << " 的账户, API: " << apiName;
        }
    }
}
void AccountManager::getAccountByUserName(string apiName, string userName, shared_ptr<Accountinfo_st>& account)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    if (accountList.find(apiName) != accountList.end() &&
        accountList[apiName].find(userName) != accountList[apiName].end()) {
        account = accountList[apiName][userName];
        if (account && account->tokenStatus) {
            account->useCount++;
            LOG_INFO << "[账户管理] 按用户名获取账户: 使用次数已增加 " << account->userName << ", 新值: " << account->useCount;
        }
    } else {
        LOG_ERROR << "[账户管理] 按用户名获取账户: 未找到账户 apiName=" << apiName << ", userName=" << userName;
        account = nullptr;
    }
}
void AccountManager::checkAccount()
{
    LOG_INFO << "[账户管理] 检查账户开始";
    //check account from accountList
    //and add to accountPoolMap
    LOG_INFO << "[账户管理] 检查账户完成";
}   
void AccountManager::refreshAccountQueue(string apiName)
{
    shared_ptr<Accountinfo_st> account = accountPoolMap[apiName]->top();
    accountPoolMap[apiName]->pop();
    accountPoolMap[apiName]->push(account);
}
void AccountManager::printAccountPoolMap()
{
    LOG_INFO << "[账户管理] 打印账户池映射开始";
    for(auto& apiName : accountPoolMap)
    {
        LOG_INFO << "[账户管理] API名称: " << apiName.first << ", 账户队列大小: " << apiName.second->size();
        auto tempQueue = *apiName.second;
       while(!tempQueue.empty())
       {
            auto account = tempQueue.top();
            LOG_INFO << "[账户管理] 用户名: " << account->userName;
            LOG_INFO << "[账户管理] 密码: " << account->passwd;
            LOG_INFO << "[账户管理] Token状态: " << account->tokenStatus;
            LOG_INFO << "[账户管理] 账户状态: " << account->accountStatus;
            LOG_INFO << "[账户管理] 用户TobitId: " << account->userTobitId;
            LOG_INFO << "[账户管理] 使用次数: " << account->useCount;
            LOG_INFO << "[账户管理] 认证Token: " << account->authToken;
            LOG_INFO << "[账户管理] --------------------------------"; 
            tempQueue.pop();
       }
    }
    LOG_INFO << "[账户管理] 打印账户池映射完成";
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
    LOG_INFO << "checkAlivableToken response: " << response->getStatusCode();
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
    
    // 先复制需要检查的账号列表，避免长时间持有锁
    std::vector<shared_ptr<Accountinfo_st>> accountsToCheck;
    {
        std::lock_guard<std::mutex> lock(accountListMutex);
        for(auto& apiName : accountList)
        {
            for(auto& userName : apiName.second)
            {
                accountsToCheck.push_back(userName.second);
            }
        }
    }
    
    // 在锁外进行检查操作
    for(auto& account : accountsToCheck)
    {
        LOG_INFO << "checkToken accountinfo: " << account->apiName << " " << account->userName;
        if(checkTokenMap[account->apiName])
        {
            bool result = (this->*checkTokenMap[account->apiName])(account->authToken);
            LOG_INFO << "checkToken result: " << result;
            setStatusTokenStatus(account->apiName, account->userName, result);
        }
        else
        {
            LOG_ERROR << "apiName: " << account->apiName << " is not supported";
        }
    }
    LOG_INFO << "checkToken end";
}

void AccountManager::updateToken()
{
    LOG_INFO << "updateToken start";
    
    // 先复制需要更新的账号列表，避免长时间持有锁
    std::vector<shared_ptr<Accountinfo_st>> accountsToUpdate;
    {
        std::lock_guard<std::mutex> lock(accountListMutex);
        for(auto& apiName : accountList)
        {
            for(auto& userName : apiName.second)
            {
                if(!userName.second->tokenStatus || userName.second->authToken.empty())
                {
                    accountsToUpdate.push_back(userName.second);
                }
            }
        }
    }
    
    // 在锁外进行更新操作
    for(auto& account : accountsToUpdate)
    {
        if(updateTokenMap[account->apiName])
        {
            (this->*updateTokenMap[account->apiName])(account);
            if(account->tokenStatus)
            {
                accountDbManager->updateAccount(*account);
                refreshAccountQueue(account->apiName);
            }
        }
        else
        {
            LOG_ERROR << "apiName: " << account->apiName << " is not supported";
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
        LOG_INFO << "Loading account from DB: " << accountinfo.userName << ", personId: " << accountinfo.personId << ", createTime: " << accountinfo.createTime << ", status: " << accountinfo.status;
        addAccount(accountinfo.apiName,accountinfo.userName,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId,accountinfo.createTime,accountinfo.accountType,accountinfo.status);
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
    std::lock_guard<std::mutex> lock(accountListMutex);
    return accountList;
}
void AccountManager::setStatusAccountStatus(string apiName,string userName,bool status)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    if(accountList.find(apiName) != accountList.end() && accountList[apiName].find(userName) != accountList[apiName].end())
    {
        accountList[apiName][userName]->accountStatus = status;
    }
}
void AccountManager::setStatusTokenStatus(string apiName,string userName,bool status)
{
    std::lock_guard<std::mutex> lock(accountListMutex);
    if(accountList.find(apiName) != accountList.end() && accountList[apiName].find(userName) != accountList[apiName].end())
    {
        accountList[apiName][userName]->tokenStatus = status;
        if(!status)
            {
                std::lock_guard<std::mutex> lock2(accountListNeedUpdateMutex);
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
            // 使用数据库统计，包含pending状态的账号
            int currentCount = accountDbManager->countAccountsByChannel(channel.channelName, true);

            LOG_INFO << "Channel: " << channel.channelName << ", Target: " << channel.accountCount << ", Current (including pending): " << currentCount;

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
    LOG_INFO << "[自动注册] 开始为渠道 " << apiName << " 自动注册账号";
    
    // Step 1: 创建待注册记录，预占位置
    int waitingId = accountDbManager->createWaitingAccount(apiName);
    if (waitingId < 0) {
        LOG_ERROR << "[自动注册] 创建待注册记录失败: " << apiName;
        return;
    }
    LOG_INFO << "[自动注册] 创建待注册账号成功, ID: " << waitingId;
    
    // Step 2: 标记为注册中状态，并加入内存追踪集合
    {
        std::lock_guard<std::mutex> lock(registeringMutex_);
        registeringAccountIds_.insert(waitingId);
    }
    accountDbManager->updateAccountStatusById(waitingId, AccountStatus::REGISTERING);
    LOG_INFO << "[自动注册] 账号状态已更新为注册中, ID: " << waitingId;
    
    // 使用 RAII 确保无论如何都会从追踪集合中移除
    struct RegisteringGuard {
        AccountManager* manager;
        int id;
        ~RegisteringGuard() {
            std::lock_guard<std::mutex> lock(manager->registeringMutex_);
            manager->registeringAccountIds_.erase(id);
            LOG_INFO << "[自动注册] 从注册中追踪集合移除, ID: " << id;
        }
    } guard{this, waitingId};
    
    string firstName = "User" + generateRandomString(5);
    string lastName = "Auto" + generateRandomString(5);
    string password = "Pwd" + generateRandomString(8) + "!";

    Json::Value requestBody;
    requestBody["first_name"] = firstName;
    requestBody["last_name"] = lastName;
    requestBody["password"] = password;

    const string fullUrl = getRegistServiceUrl("chaynsapi");
    if (fullUrl.empty()) {
        LOG_ERROR << "[自动注册] 未找到 chaynsapi 的注册服务URL";
        // 注册失败，删除待注册记录
        accountDbManager->deleteWaitingAccount(waitingId);
        return;
    }

    // 解析 URL
    string baseUrl, path;
    size_t protocolPos = fullUrl.find("://");
    if (protocolPos == string::npos) {
        LOG_ERROR << "[自动注册] 无效的注册服务URL格式: " << fullUrl;
        accountDbManager->deleteWaitingAccount(waitingId);
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
    LOG_INFO << "[自动注册] baseUrl: " << baseUrl;

    auto client = HttpClient::newHttpClient(baseUrl);
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(HttpMethod::Post);
    request->setPath(path);
    request->setContentTypeString("application/json");
    request->setBody(requestBody.toStyledString());
    
    LOG_INFO << "[自动注册] 发送注册请求...";
    auto [result, response] = client->sendRequest(request, 300.0);

    if (result != ReqResult::Ok || response->getStatusCode() != 200) {
        LOG_ERROR << "[自动注册] 注册请求失败. Result: " << (int)result
                  << ", Status: " << (response ? response->getStatusCode() : 0)
                  << ", Body: " << (response ? response->getBody() : "");
        // 注册失败，删除待注册记录（状态已经是 registering，需要先改回 waiting 才能删除）
        accountDbManager->updateAccountStatusById(waitingId, AccountStatus::WAITING);
        accountDbManager->deleteWaitingAccount(waitingId);
        return;
    }

    Json::Value jsonResponse;
    Json::CharReaderBuilder reader;
    string errs;
    std::string responseBody(response->getBody());
    std::istringstream s(responseBody);
    
    if (!Json::parseFromStream(reader, s, &jsonResponse, &errs)) {
        LOG_ERROR << "[自动注册] 解析注册响应失败: " << errs;
        // 解析失败，删除待注册记录
        accountDbManager->updateAccountStatusById(waitingId, AccountStatus::WAITING);
        accountDbManager->deleteWaitingAccount(waitingId);
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

    LOG_INFO << "[自动注册] 注册成功: " << email;
    
    string createTime = trantor::Date::now().toDbStringLocal();
    string accountType = hasProAccess ? "pro" : "free";

    // Step 3: 注册成功，激活账号记录
    Accountinfo_st newAccount(apiName, email, respPassword, token, 0, true, true, userid, personid, createTime, accountType, AccountStatus::ACTIVE);
    
    if (accountDbManager->activateAccount(waitingId, newAccount)) {
        LOG_INFO << "[自动注册] 账号激活成功: " << email;
        addAccount(apiName, email, respPassword, token, 0, true, true, userid, personid, createTime, accountType, AccountStatus::ACTIVE);
        
        // 自动注册账号后，检查并更新该账号的 accountType
        shared_ptr<Accountinfo_st> newAccountPtr;
        {
            std::lock_guard<std::mutex> lock(accountListMutex);
            if (accountList.find(apiName) != accountList.end() &&
                accountList[apiName].find(email) != accountList[apiName].end()) {
                newAccountPtr = accountList[apiName][email];
            }
        }
        if (newAccountPtr) {
            LOG_INFO << "[自动注册] 检查账号类型: " << email;
            updateAccountType(newAccountPtr);
        }
    } else {
        LOG_ERROR << "[自动注册] 账号激活失败, ID: " << waitingId;
        // 激活失败，删除待注册记录（以防万一）
        accountDbManager->updateAccountStatusById(waitingId, AccountStatus::WAITING);
        accountDbManager->deleteWaitingAccount(waitingId);
    }
}

// 从上游服务删除账号
// 流程: 1) 获取确认 token  2) 使用确认 token 删除账号
bool AccountManager::deleteUpstreamAccount(const Accountinfo_st& account)
{
    LOG_INFO << "[上游删除] 开始删除上游账号: " << account.userName;

    // 检查必要字段
    if (account.userName.empty() || account.passwd.empty()) {
        LOG_ERROR << "[上游删除] 缺少用户名或密码，无法删除上游账号";
        return false;
    }
    if (account.authToken.empty()) {
        LOG_ERROR << "[上游删除] 缺少 authToken，无法删除上游账号: " << account.userName;
        return false;
    }

    // ====== 第一步: 获取确认 token ======
    // Basic 认证: base64(username:password)
    string credentials = account.userName + ":" + account.passwd;
    string base64Credentials = drogon::utils::base64Encode(
        reinterpret_cast<const unsigned char*>(credentials.data()),
        credentials.size()
    );

    try {
        auto authClient = HttpClient::newHttpClient("https://auth.tobit.com");
        auto authRequest = HttpRequest::newHttpRequest();
        authRequest->setMethod(HttpMethod::Post);
        authRequest->setPath("/v2/token");
        authRequest->addHeader("Authorization", "Basic " + base64Credentials);
        authRequest->setContentTypeString("application/json; charset=utf-8");

        Json::Value tokenBody;
        tokenBody["tokenType"] = 12;
        tokenBody["isConfirmation"] = false;
        tokenBody["locationId"] = 234191;
        tokenBody["deviceId"] = "1a0e1c3b-bc2e-4dd8-863a-e7061e35ccff";
        tokenBody["createIfNotExists"] = false;
        authRequest->setBody(tokenBody.toStyledString());

        LOG_INFO << "[上游删除] 请求确认 token...";
        auto [authResult, authResponse] = authClient->sendRequest(authRequest, 30.0);

        if (authResult != ReqResult::Ok || !authResponse || authResponse->getStatusCode() != 200) {
            LOG_ERROR << "[上游删除] 获取确认 token 失败. Result: " << (int)authResult
                      << ", Status: " << (authResponse ? authResponse->getStatusCode() : 0)
                      << ", Body: " << (authResponse ? std::string(authResponse->getBody()) : "");
            return false;
        }

        // 解析确认 token
        Json::CharReaderBuilder reader;
        Json::Value authJson;
        string errs;
        string authBody = string(authResponse->getBody());
        istringstream authStream(authBody);

        if (!Json::parseFromStream(reader, authStream, &authJson, &errs)) {
            LOG_ERROR << "[上游删除] 解析确认 token 响应失败: " << errs;
            return false;
        }

        string confirmationToken = authJson["token"].asString();
        if (confirmationToken.empty()) {
            LOG_ERROR << "[上游删除] 确认 token 为空";
            return false;
        }
        LOG_INFO << "[上游删除] 获取确认 token 成功";

        // ====== 第二步: 删除账号 ======
        auto deleteClient = HttpClient::newHttpClient("https://webapi.tobit.com");
        auto deleteRequest = HttpRequest::newHttpRequest();
        deleteRequest->setMethod(HttpMethod::Delete);
        deleteRequest->setPath("/AccountService/v1.0/chayns/User");
        deleteRequest->addHeader("Authorization", "Bearer " + account.authToken);
        deleteRequest->addHeader("x-confirmation-token", "bearer " + confirmationToken);
        deleteRequest->setContentTypeString("application/json");

        Json::Value deleteBody;
        deleteBody["PersonId"] = account.personId;
        deleteBody["ForceDelete"] = true;
        deleteRequest->setBody(deleteBody.toStyledString());

        LOG_INFO << "[上游删除] 发送删除请求...";
        auto [delResult, delResponse] = deleteClient->sendRequest(deleteRequest, 30.0);

        if (delResult != ReqResult::Ok || !delResponse || delResponse->getStatusCode() != 200) {
            LOG_ERROR << "[上游删除] 删除上游账号失败. Result: " << (int)delResult
                      << ", Status: " << (delResponse ? delResponse->getStatusCode() : 0)
                      << ", Body: " << (delResponse ? std::string(delResponse->getBody()) : "");
            return false;
        }

        LOG_INFO << "[上游删除] 上游账号删除成功: " << account.userName;

        // ====== 第三步: 撤销 token (logout / invalidate) ======
        try {
            auto invalidClient = HttpClient::newHttpClient("https://auth.tobit.com");
            auto invalidRequest = HttpRequest::newHttpRequest();
            invalidRequest->setMethod(HttpMethod::Post);
            invalidRequest->setPath("/v2/invalidToken");
            invalidRequest->setContentTypeString("application/json; charset=utf-8");

            Json::Value invalidBody;
            invalidBody["token"] = account.authToken;
            invalidRequest->setBody(invalidBody.toStyledString());

            LOG_INFO << "[上游删除] 撤销 token...";
            auto [invResult, invResponse] = invalidClient->sendRequest(invalidRequest, 30.0);

            if (invResult == ReqResult::Ok && invResponse && invResponse->getStatusCode() == 200) {
                LOG_INFO << "[上游删除] token 撤销成功: " << account.userName;
            } else {
                LOG_WARN << "[上游删除] token 撤销失败（账号已删除，非致命）. Status: "
                         << (invResponse ? invResponse->getStatusCode() : 0);
            }
        } catch (const std::exception& ex) {
            LOG_WARN << "[上游删除] token 撤销异常（账号已删除，非致命）: " << ex.what();
        }

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "[上游删除] 异常: " << e.what() << ", 用户: " << account.userName;
        return false;
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
    
    // 先复制需要更新的账号列表，避免长时间持有锁
    std::vector<shared_ptr<Accountinfo_st>> accountsToUpdate;
    {
        std::lock_guard<std::mutex> lock(accountListMutex);
        for (auto& apiName : accountList) {
            for (auto& userName : apiName.second) {
                auto account = userName.second;
                // 只更新 token 有效的账号
                if (account && account->tokenStatus && !account->authToken.empty()) {
                    accountsToUpdate.push_back(account);
                }
            }
        }
    }
    
    // 在锁外进行更新操作
    for (auto& account : accountsToUpdate) {
        updateAccountType(account);
        // 添加短暂延迟，避免请求过于频繁
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    LOG_INFO << "updateAllAccountTypes end";
}

// 启动定时检查 accountType 的线程
void AccountManager::checkAccountTypeThread()
{
    std::thread t([this]() {
        // 启动后等待 5 分钟再执行第一次检查，让系统稳定
        std::this_thread::sleep_for(std::chrono::minutes(1));
        
        while (true) {
            LOG_INFO << "Starting scheduled account type check...";
            updateAllAccountTypes();
            
            // 每 1 小时检查一次
            std::this_thread::sleep_for(std::chrono::hours(3));
        }
    });
    t.detach();
    LOG_INFO << "checkAccountTypeThread started";
}

// 检查账号是否正在注册中（通过ID）
bool AccountManager::isAccountRegistering(int pendingId)
{
    std::lock_guard<std::mutex> lock(registeringMutex_);
    return registeringAccountIds_.find(pendingId) != registeringAccountIds_.end();
}

// 检查账号是否正在注册中（通过用户名）
bool AccountManager::isAccountRegisteringByUsername(const string& userName)
{
    // 检查数据库中的状态
    string status = accountDbManager->getAccountStatusByUsername("chaynsapi", userName);
    return status == AccountStatus::REGISTERING;
}

