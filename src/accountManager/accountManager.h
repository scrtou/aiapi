#ifndef ACCOUNT_MANAGER_H
#define ACCOUNT_MANAGER_H
#include <string>
#include <memory>
#include <queue>
#include <vector>
#include <map>
#include <list>
#include <set>
#include <APIinterface.h>
#include <../dbManager/account/accountDbManager.h>
using namespace std;
using namespace drogon;
class AccountDbManager;
// 账号状态常量
namespace AccountStatus {
    const string WAITING = "waiting";       // 待注册（已创建占位记录）
    const string REGISTERING = "registering"; // 注册中（HTTP请求已发送）
    const string ACTIVE = "active";         // 正常激活
    const string DISABLED = "disabled";     // 已禁用
}

struct Accountinfo_st
{
    string apiName;
    string userName;
    string passwd;
    string authToken;
    int useCount;
    bool tokenStatus=false;
    bool accountStatus=false;
    int userTobitId;
    string personId;
    string createTime;
    string accountType;  // 账号类型: "pro" 或 "free"
    string status;       // 账号状态: "pending", "active", "disabled"

    Accountinfo_st(){}
    Accountinfo_st(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId,string personId,string createTime="",string accountType="free",string status="active")
    {
        this->apiName = apiName;
        this->userName = userName;
        this->passwd = passwd;
        this->authToken = authToken;
        this->useCount = useCount;
        this->tokenStatus = tokenStatus;
        this->accountStatus = accountStatus;
        this->userTobitId = userTobitId;
        this->personId = personId;
        this->createTime = createTime;
        this->accountType = accountType;
        this->status = status;
    }

    // 将请求/数据库中的 JSON 字段解析为统一账号结构（仅保留 camelCase 新命名）。
    static Accountinfo_st fromJson(const Json::Value& value)
    {
        Accountinfo_st result;
        result.apiName = value.get("apiName", "").asString();
        result.userName = value.get("userName", "").asString();
        result.passwd = value.get("password", "").asString();
        result.authToken = value.get("authToken", "").asString();
        result.useCount = value.get("useCount", 0).asInt();
        result.tokenStatus = value.get("tokenStatus", false).asBool();
        result.accountStatus = value.get("accountStatus", false).asBool();
        result.userTobitId = value.get("userTobitId", 0).asInt();
        result.personId = value.get("personId", "").asString();
        result.createTime = value.get("createTime", "").asString();
        result.accountType = value.get("accountType", "free").asString();
        result.status = value.get("status", "active").asString();
        return result;
    }

    // 将账号结构导出为统一 JSON（仅保留 camelCase 新命名）。
    Json::Value toJson() const
    {
        Json::Value value;
        value["apiName"] = apiName;
        value["userName"] = userName;
        value["password"] = passwd;
        value["authToken"] = authToken;
        value["useCount"] = useCount;
        value["tokenStatus"] = tokenStatus;
        value["accountStatus"] = accountStatus;
        value["userTobitId"] = userTobitId;
        value["personId"] = personId;
        value["createTime"] = createTime;
        value["accountType"] = accountType;
        value["status"] = status;
        return value;
    }
};

struct AccountCompare
{
    bool operator()(const shared_ptr<Accountinfo_st>& a, const shared_ptr<Accountinfo_st>& b)
    {
        if(a->tokenStatus!=b->tokenStatus)return b->tokenStatus;
        return a->useCount > b->useCount;
    }
};
//定义函数指针

class AccountManager
{
    private:
   // 旧单例写法保留：当前已改为函数内静态对象
    map<string,shared_ptr<priority_queue<shared_ptr<Accountinfo_st>,vector<shared_ptr<Accountinfo_st>>,AccountCompare>>> accountPoolMap;
    map<string,map<string,shared_ptr<Accountinfo_st>>> accountList;// 二级索引结构：apiName -> userName -> accountInfo
    mutable std::mutex accountListMutex;  // 保护 accountList 的互斥锁
    std::set<int> registeringAccountIds_;     // 正在注册中的账号ID集合
    mutable std::mutex registeringMutex_;     // 保护 registeringAccountIds_ 的互斥锁
    list<shared_ptr<Accountinfo_st>> accountListNeedUpdate;//需要更新的账号,
    std::mutex accountListNeedUpdateMutex;
    std::condition_variable accountListNeedUpdateCondition;
     // 
    map<string, void (AccountManager::*)(shared_ptr<Accountinfo_st>)> updateTokenMap = {
        {"chaynsapi", &AccountManager::updateChaynsToken}
    };

    map<string, bool (AccountManager::*)(string)> checkTokenMap = {
        {"chaynsapi", &AccountManager::checkChaynsToken}
    };
    // 旧字段保留：历史版本用于缓存 API 名称列表
    shared_ptr<AccountDbManager> accountDbManager;
     AccountManager();
    ~AccountManager();

    public:
    static AccountManager& getInstance()
    {
        static AccountManager instance;
        return instance;
    }
    AccountManager(const AccountManager&) = delete;
    AccountManager& operator=(const AccountManager&) = delete;
    void init();
    void loadAccount();
    void saveAccount();

    void addAccount(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId,string personId,string createTime="",string accountType="free",string status="active");
    bool addAccountbyPost(Accountinfo_st accountinfo);
    bool updateAccount(Accountinfo_st accountinfo);
    bool deleteAccountbyPost(string apiName,string userName);
    void getAccount(string apiName,shared_ptr<Accountinfo_st>& account, string accountType = "");
    void getAccountByUserName(string apiName, string userName, shared_ptr<Accountinfo_st>& account);
    void checkAccount();
    void checkToken();
    void updateToken();
    void updateChaynsToken(shared_ptr<Accountinfo_st> accountinfo);
    bool checkChaynsToken(string token);
    Json::Value getChaynsToken(string username,string passwd);
    void registerAPIinterface(string apiName,shared_ptr<APIinterface> apiInterface);
    void refreshAccountQueue(string apiName);
    void printAccountPoolMap();
    void checkUpdateTokenthread();
    void checkUpdateAccountToken();
    bool isServerReachable(const string& host, int maxRetries = 300);
    void loadAccountFromDatebase();
    void saveAccountToDatebase();
    void loadAccountFromConfig();
    void setStatusAccountStatus(string apiName,string userName,bool status);
    void setStatusTokenStatus(string apiName,string userName,bool status);
    std::map<string,map<string,shared_ptr<Accountinfo_st>>> getAccountList();

    void waitUpdateAccountToken();
    void waitUpdateAccountTokenThread();

    void checkChannelAccountCounts();
    void autoRegisterAccount(string apiName);
    void checkAccountCountThread();
    
    // 竞态条件保护相关方法
    bool isAccountRegistering(int pendingId);  // 检查账号是否正在注册中
    bool isAccountRegisteringByUsername(const string& userName);  // 通过用户名检查
    
    // 上游账号删除
    bool deleteUpstreamAccount(const Accountinfo_st& account);  // 从上游服务删除账号
    
    // 定时更新账号类型相关方法
    bool getUserProAccess(const string& token, const string& personId);  // 获取用户 Pro 权限状态
    void updateAccountType(shared_ptr<Accountinfo_st> account);  // 更新单个账号的 accountType
    void updateAllAccountTypes();  // 更新所有账号的 accountType
    void checkAccountTypeThread();  // 启动定时检查 accountType 的线程
    void cleanExpiredAccounts();  // 自动清理创建超过6天的过期账号
};
#endif