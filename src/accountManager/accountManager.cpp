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
    // 旧方案：直接触发一次令牌更新（已保留注释以便排查）
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
    // 旧设计备注：可从配置文件加载账号，当前优先从数据库加载
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
    // 调试入口：如需排查账号池分配，可临时开启打印
}
void AccountManager::loadAccountFromConfig()
{
    LOG_INFO << "[账户管理] 从配置文件加载账户开始";
     auto customConfig = app().getCustomConfig();
    auto configAccountList = customConfig["account"];

    // 将配置中的账号逐条写入内存账号池，供调度器按权重获取
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
    // 预留检查点：可在此加入 accountList 与账号池一致性校验
    // 将配置中的账号逐条写入内存账号池，供调度器按权重获取
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
    LOG_INFO << "[账户管理] 开始校验 Chayns 令牌";
    auto client = HttpClient::newHttpClient("https://webapi.tobit.com/AccountService/v1.0/Chayns/User");
    auto request = HttpRequest::newHttpRequest();
    request->setMethod(HttpMethod::Get);
    request->setPath("/AccountService/v1.0/Chayns/User");
    request->addHeader("Authorization","Bearer " + token);
    auto [result, response] = client->sendRequest(request);
    LOG_INFO << "[账户管理] 令牌校验接口响应状态码: " << response->getStatusCode();
    LOG_INFO << "[账户管理] Chayns 令牌校验结束";
    if(response->getStatusCode()!=200)
    {
        return false;
    }
    return true;
}
Json::Value AccountManager::getChaynsToken(string username,string passwd)
{
    LOG_INFO << "[账户管理] 开始获取 Chayns 令牌";
    const string fullUrl = getLoginServiceUrl("chaynsapi");
    if (fullUrl.empty()) {
        LOG_ERROR << "[账户管理] 配置中未找到 chaynsapi 的登录服务地址";
        return Json::Value();
    }
    LOG_INFO << "[账户管理] 完整登录地址: "<<fullUrl;

    // 解析 URL
    string baseUrl, path;
    size_t protocolPos = fullUrl.find("://");
    if (protocolPos == string::npos) {
        LOG_ERROR << "[账户管理] 登录服务地址格式无效: " << fullUrl;
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
    LOG_INFO << "[账户管理] 解析出的主机地址: "<<baseUrl;

     // 等待服务器可用
    if (!isServerReachable(baseUrl)) {
        LOG_ERROR << "[账户管理] 达到最大重试次数后仍无法连通目标主机: " << baseUrl;
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
        LOG_INFO << "[账户管理] 登录服务原始响应内容: " << body;
    }
    string errs;
    istringstream s(body);
    if (!Json::parseFromStream(reader, s, &responsejson, &errs)) {
        LOG_ERROR << "[账户管理] 解析登录响应 JSON 失败: " << errs;
    }
    return responsejson;
}
void AccountManager::checkToken()
{
    LOG_INFO << "[账户管理] 开始批量校验账号令牌";
    LOG_INFO << "[账户管理] 令牌校验函数映射数量: " << checkTokenMap.size();
    
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
        LOG_INFO << "[账户管理] 正在校验账号: " << account->apiName << " " << account->userName;
        if(checkTokenMap[account->apiName])
        {
            bool result = (this->*checkTokenMap[account->apiName])(account->authToken);
            LOG_INFO << "[账户管理] 令牌校验结果: " << result;
            setStatusTokenStatus(account->apiName, account->userName, result);
        }
        else
        {
            LOG_ERROR << "[账户管理] 不支持的上游渠道 apiName: " << account->apiName << " is not supported";
        }
    }
    LOG_INFO << "[账户管理] 批量令牌校验结束";
}

void AccountManager::updateToken()
{
    LOG_INFO << "[账户管理] 开始批量更新令牌";
    
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
            LOG_ERROR << "[账户管理] 不支持的上游渠道 apiName: " << account->apiName << " is not supported";
        }
    }
    LOG_INFO << "[账户管理] 批量令牌更新结束";
}

void AccountManager::updateChaynsToken(shared_ptr<Accountinfo_st> accountinfo)
{
    LOG_INFO << "[账户管理] 开始更新 Chayns 令牌，用户: " << accountinfo->userName;
    auto token = getChaynsToken(accountinfo->userName,accountinfo->passwd);
    LOG_INFO << "[账户管理] Chayns 令牌更新结果: " << (token.empty()?"empty":"not empty");
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
    LOG_INFO << "[账户管理] Chayns 令牌更新流程结束";
}
void AccountManager::checkUpdateTokenthread()
{
    thread t1([&]{
        while(true)
        {
            checkToken();
            // 清理创建超过6天的过期账号
            cleanExpiredAccounts();
            this_thread::sleep_for(chrono::hours(5));
        }
    });
    t1.detach();
}
void AccountManager::checkUpdateAccountToken()  
{
    checkToken();
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
                LOG_INFO << "[账户管理] 目标主机已连通，累计重试次数: " << retryCount << " 次";
                return true;
            }
        } catch (...) {
            LOG_INFO << "[账户管理] 目标主机暂不可达，准备第 N 次重试: " << retryCount + 1;
        }
        
        retryCount++;
        std::this_thread::sleep_for(std::chrono::seconds(1)); // 等待1秒后重试
    }
    
    return false;
}
void AccountManager::loadAccountFromDatebase()
{
    LOG_INFO << "[账户管理] 开始从数据库加载账号";
    auto accountDBList = accountDbManager->getAccountDBList();
    for(auto& accountinfo:accountDBList)
    {
        LOG_INFO << "[账户管理] 从数据库加载账号记录: " << accountinfo.userName << ", personId: " << accountinfo.personId << ", createTime: " << accountinfo.createTime << ", status: " << accountinfo.status;
        addAccount(accountinfo.apiName,accountinfo.userName,accountinfo.passwd,accountinfo.authToken,accountinfo.useCount,accountinfo.tokenStatus,accountinfo.accountStatus,accountinfo.userTobitId,accountinfo.personId,accountinfo.createTime,accountinfo.accountType,accountinfo.status);
    }
    LOG_INFO << "[账户管理] 数据库加载完成，账号总数: "<<accountDBList.size();
}
void AccountManager::saveAccountToDatebase()
{
    LOG_INFO << "[账户管理] 开始将账号写回数据库";
    for(auto& apiName:accountList)
    {
        for(auto& userName:apiName.second)
        {
            accountDbManager->addAccount(*(userName.second.get()));
        }
    }
    LOG_INFO << "[账户管理] 账号写回数据库完成";
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
    LOG_INFO << "[账户管理] 账号令牌更新线程已启动";
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
            LOG_INFO << "[账户管理] 正在处理账号更新，用户: " << account->userName;
        }

        // 验证账号
        if (!account) {
            LOG_ERROR << "[账户管理] 账号指针无效，跳过本次更新";
            continue;
        }

        // 查找更新函数
        auto updateFunc = updateTokenMap.find(account->apiName);
        if (updateFunc == updateTokenMap.end()) {
            LOG_ERROR << "[账户管理] 不支持的 API 名称: " << account->apiName;
            continue;
        }

        try {
            // 执行token更新
            (this->*(updateFunc->second))(account);

            // 仅在更新成功时更新数据库和刷新队列
            if (account->tokenStatus) {
                if (!accountDbManager->updateAccount(*account)) {
                    LOG_ERROR << "[账户管理] 更新数据库账号记录失败";
                    continue;
                }
                refreshAccountQueue(account->apiName);
                
                // Token 更新成功后，检查并更新 accountType
                LOG_INFO << "[账户管理] 令牌更新完成，开始校验账号类型，用户: " << account->userName;
                // 若需要强制刷新账号类型，可在此处启用单账号更新
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR << "[账户管理] 执行账号更新时发生异常: " << e.what() 
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
            LOG_INFO << "[账户管理] 开始检查各渠道账号数量";
            checkChannelAccountCounts();
            // 定时巡检周期：每 10 分钟执行一次账号数量检查
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

            LOG_INFO << "[账户管理] 渠道状态 -> 名称: " << channel.channelName << "，目标数量: " << channel.accountCount << "，当前数量（含待注册）: " << currentCount;

            if(currentCount < channel.accountCount)
            {
                int needed = channel.accountCount - currentCount;
                LOG_INFO << "[账户管理] 该渠道需补充注册账号数量: " << needed << "，渠道: " << channel.channelName;
                // 串行注册：逐个补充账号，避免瞬时请求压垮上游服务
                for(int i=0; i<needed; ++i)
                {
                    autoRegisterAccount(channel.channelName);
                    // 注册节流：每次注册之间增加短暂间隔，降低触发限流风险
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
    LOG_INFO << "[账户管理] 开始查询 Pro 权限，personId: " << personId;
    
    // 接口地址示例（保留 URL 便于定位）：https://cube.tobit.cloud/ai-proxy/v1/userSettings/personId/{personId}
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
            LOG_ERROR << "[账户管理] 查询 Pro 权限请求失败";
            return false;
        }
        
        LOG_INFO << "[账户管理] 查询 Pro 权限响应状态码: " << response->getStatusCode();
        
        if (response->getStatusCode() == 200) {
            Json::CharReaderBuilder reader;
            Json::Value jsonResponse;
            string errs;
            string body = string(response->getBody());
            istringstream s(body);
            
            if (Json::parseFromStream(reader, s, &jsonResponse, &errs)) {
                if (jsonResponse.isMember("hasProAccess")) {
                    bool hasProAccess = jsonResponse["hasProAccess"].asBool();
                    LOG_INFO << "[账户管理] 查询 Pro 权限结果，hasProAccess=" << hasProAccess;
                    return hasProAccess;
                }
            } else {
                LOG_ERROR << "[账户管理] 解析 Pro 权限响应 JSON 失败: " << errs;
            }
        } else {
            LOG_ERROR << "[账户管理] 查询 Pro 权限接口返回错误: " << response->getStatusCode()
                      << " - " << response->getBody();
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "[账户管理] 查询 Pro 权限时捕获异常: " << e.what();
    }
    
    return false;
}

// 更新单个账号的 accountType
void AccountManager::updateAccountType(shared_ptr<Accountinfo_st> account)
{
    if (!account || account->authToken.empty() || account->personId.empty()) {
        LOG_ERROR << "[账户管理] 更新账号类型失败：账号数据无效";
        return;
    }
    
    LOG_INFO << "[账户管理] 开始更新账号类型，用户: " << account->userName;
    
    bool hasProAccess = getUserProAccess(account->authToken, account->personId);
    string newAccountType = hasProAccess ? "pro" : "free";
    
    if (account->accountType != newAccountType) {
        LOG_INFO << "[账户管理] 账号类型发生变化，用户: " << account->userName
                 << ": " << account->accountType << " -> " << newAccountType;
        account->accountType = newAccountType;
        
        // 更新数据库
        if (accountDbManager->updateAccount(*account)) {
            LOG_INFO << "[账户管理] 账号类型已写入数据库，用户: " << account->userName;
        } else {
            LOG_ERROR << "[账户管理] 账号类型写入数据库失败，用户: " << account->userName;
        }
    } else {
        LOG_INFO << "[账户管理] 账号类型未变化，用户: " << account->userName << ": " << account->accountType;
    }
}

// 更新所有账号的 accountType
void AccountManager::updateAllAccountTypes()
{
    LOG_INFO << "[账户管理] 开始全量刷新账号类型";
    
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
    
    LOG_INFO << "[账户管理] 全量刷新账号类型结束";
}

// 启动定时检查 accountType 的线程
void AccountManager::checkAccountTypeThread()
{
    std::thread t([this]() {
        // 启动后等待 5 分钟再执行第一次检查，让系统稳定
        std::this_thread::sleep_for(std::chrono::minutes(1));
        
        while (true) {
            LOG_INFO << "[账户管理] 启动定时账号类型巡检任务";
            updateAllAccountTypes();
            
            // 每 3 小时检查一次
            std::this_thread::sleep_for(std::chrono::hours(3));
        }
    });
    t.detach();
    LOG_INFO << "[账户管理] 账号类型巡检线程已启动";
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

void AccountManager::cleanExpiredAccounts()
{
    LOG_INFO << "[自动清理] 开始检查过期账号...";
    
    // 获取当前时间
    auto now = trantor::Date::now();
    // 6天 = 6 * 24 * 3600 秒
    const double expireDurationSeconds = 6.0 * 24.0 * 3600.0;
    
    // 获取当前所有账号的快照
    auto currentAccountMap = getAccountList();
    
    list<Accountinfo_st> expiredAccounts;
    
    for (auto &apiPair : currentAccountMap)
    {
        for (auto &accountPair : apiPair.second)
        {
            auto &account = accountPair.second;
            if (!account) continue;
            
            // 只清理 free 类型的账号
            if (account->accountType != "free") continue;
            
            // 跳过正在注册中的账号
            if (account->status == AccountStatus::REGISTERING) continue;
            
            // 检查 createTime 是否为空
            if (account->createTime.empty()) {
                LOG_WARN << "[自动清理] 账号 " << account->userName << " 没有 createTime，跳过";
                continue;
            }
            
            // 解析 createTime (格式: "2026-02-07 12:46:38")
            auto createDate = trantor::Date::fromDbStringLocal(account->createTime);
            
            // 计算时间差（秒）
            double ageSec = now.secondsSinceEpoch() - createDate.secondsSinceEpoch();
            
            if (ageSec >= expireDurationSeconds)
            {
                LOG_INFO << "[自动清理] 账号 " << account->userName
                         << " 创建于 " << account->createTime
                         << "，已超过6天（" << (ageSec / 86400.0) << "天），标记为待删除";
                
                // 复制完整账号信息用于删除
                Accountinfo_st expiredAccount;
                expiredAccount.apiName = account->apiName;
                expiredAccount.userName = account->userName;
                expiredAccount.passwd = account->passwd;
                expiredAccount.authToken = account->authToken;
                expiredAccount.userTobitId = account->userTobitId;
                expiredAccount.personId = account->personId;
                expiredAccount.createTime = account->createTime;
                expiredAccount.accountType = account->accountType;
                expiredAccount.status = account->status;
                
                expiredAccounts.push_back(expiredAccount);
            }
        }
    }
    
    if (expiredAccounts.empty()) {
        LOG_INFO << "[自动清理] 没有发现过期账号";
        return;
    }
    
    LOG_INFO << "[自动清理] 发现 " << expiredAccounts.size() << " 个过期账号，开始删除...";
    
    for (auto &account : expiredAccounts)
    {
        // 1) 从内存中删除
        if (!deleteAccountbyPost(account.apiName, account.userName)) {
            LOG_WARN << "[自动清理] 从内存删除失败: " << account.userName;
            continue;
        }
        
        // 2) 从上游删除账号
        bool upstreamDeleted = deleteUpstreamAccount(account);
        if (upstreamDeleted) {
            LOG_INFO << "[自动清理] 上游账号删除成功: " << account.userName;
        } else {
            LOG_WARN << "[自动清理] 上游账号删除失败（继续删除本地数据库）: " << account.userName;
        }
        
        // 3) 从本地数据库删除
        accountDbManager->deleteAccount(account.apiName, account.userName);
        LOG_INFO << "[自动清理] 数据库记录已删除: " << account.userName;
    }
    
    // 重新加载账号
    loadAccount();
    
    // 检查渠道账号数量（可能需要补充账号）
    checkChannelAccountCounts();
    
    LOG_INFO << "[自动清理] 过期账号清理完成，共删除 " << expiredAccounts.size() << " 个账号";
}

