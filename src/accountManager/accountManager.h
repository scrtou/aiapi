#ifndef ACCOUNT_MANAGER_H
#define ACCOUNT_MANAGER_H
#include <string>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <queue>
#include <vector>
#include <map>
#include <list>
#include <set>
#include <cstdint>
#include "domain/port/APIinterface.h"
using namespace std;

// 纯数据类型（Accountinfo_st / AccountCompare / AccountAutomationSettings /
// AccountRequirement / AccountStatus）已下沉至中立层，见该文件头注释。
#include "domain/model/AccountData.h"
#include <domain/port/IAccountStore.h>
#include <domain/port/IChannelStore.h>


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
    AccountAutomationSettings accountAutomationSettings_;
    mutable std::mutex accountAutomationSettingsMutex_;
    void normalizeNexosAccountsInDatabase();
     // 
    map<string, void (AccountManager::*)(shared_ptr<Accountinfo_st>)> updateTokenMap = {
        {"chaynsapi", &AccountManager::updateChaynsToken},
        {"nexosapi", &AccountManager::updateNexosToken}
    };

    map<string, bool (AccountManager::*)(string)> checkTokenMap = {
        {"chaynsapi", &AccountManager::checkChaynsToken},
        {"nexosapi", &AccountManager::checkNexosToken}
    };
    // 旧字段保留：历史版本用于缓存 API 名称列表
    // R4 试点 C：只持端口，不持 AccountDbManager 具体实现。
    // 未注入时 requireStore() 回退到 NullAccountStore（见 .cpp），不崩溃但留可诊断日志。
    shared_ptr<IAccountStore> accountDbManager;
    IAccountStore* requireStore();

    // R4 续：渠道列表来源。改造前是 ChannelDbManager::getInstance() 直呼（3 处）。
    // 复用试点 B 已有的 IChannelStore 端口，未新造抽象。
    // 命名不与上面的 setStore 重载：AccountManager 有两条独立接线，
    // 同名会让 main.cc 意图含糊，也让启动接线门禁难以分辨。
    shared_ptr<IChannelStore> channelStore;
    IChannelStore* requireChannelStore();
     AccountManager();
    ~AccountManager();

    public:
    // R4 试点 C 注入点：必须在 init() 之前调用（init() 会立刻建表/迁移账号）。
    void setStore(std::shared_ptr<IAccountStore> store);
    void setChannelStore(std::shared_ptr<IChannelStore> store);

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

    void addAccount(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId,string personId,string createTime="",string accountType="free",string status="active",std::int64_t workspaceUacId=0);
    bool addAccountbyPost(Accountinfo_st accountinfo);
    bool updateAccount(Accountinfo_st accountinfo);
    bool deleteAccountbyPost(string apiName,string userName);
    void getAccount(string apiName,shared_ptr<Accountinfo_st>& account, string accountType = "");
    bool getEligibleAccount(const string& apiName,
                            shared_ptr<Accountinfo_st>& account,
                            AccountRequirement requirement,
                            const set<string>& excludedUsers = {});
    void getAccountByUserName(string apiName, string userName, shared_ptr<Accountinfo_st>& account);
    void checkAccount();
    void checkToken();
    void updateToken();
    void updateChaynsToken(shared_ptr<Accountinfo_st> accountinfo);
    bool checkChaynsToken(string token);
    void updateNexosToken(shared_ptr<Accountinfo_st> accountinfo);
    bool checkNexosToken(string token);
    Json::Value getChaynsToken(string username,string passwd);
    Json::Value getNexosToken(string username,string passwd);
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
    void loadAccountAutomationSettings();
    AccountAutomationSettings getAccountAutomationSettings() const;
    bool updateAccountAutomationSettings(const AccountAutomationSettings& settings,
                                         bool persistToConfig = true,
                                         std::string* errorMessage = nullptr);
    void waitUpdateAccountToken();
    void waitUpdateAccountTokenThread();

    void checkChannelAccountCounts();
    void checkChannelAccountCount(string apiName);
    bool autoRegisterAccount(string apiName);
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
    void cleanExpiredAccounts();  // 自动清理创建超过配置天数的过期账号

    // ========== N4: 后台线程统一停机 ==========
    /// 停止全部账号管理后台线程并 join。幂等；未启动的线程直接跳过。
    ///
    /// 背景：原先 4 个线程全部 detach，进程退出时被强行截断。三个定时线程
    /// （token 巡检 / 账号数量巡检 / accountType 巡检）睡眠期长达 3~5 小时，
    /// 若只置标志位不唤醒，停机要等满一个周期，因此统一用 cv 可中断睡眠。
    /// waitUpdateAccountToken 阻塞在 accountListNeedUpdateCondition 上，
    /// 靠 notify_all + 谓词复查退出。
    void stopBackgroundThreads();

  private:
    /// 可中断睡眠：睡满 seconds 返回 true；被停机唤醒立即返回 false。
    /// 所有定时线程的 sleep 必须走这里，否则停机会被拖到一个完整周期。
    bool backgroundSleep(std::chrono::seconds seconds);

    std::atomic<bool>       backgroundStopRequested_{false};
    std::mutex              backgroundWakeMutex_;
    std::condition_variable backgroundWakeCv_;
    std::thread             tokenCheckThread_;     ///< checkUpdateTokenthread
    std::thread             tokenUpdateWorker_;    ///< waitUpdateAccountTokenThread
    std::thread             accountCountThread_;   ///< checkAccountCountThread
    std::thread             accountTypeThread_;    ///< checkAccountTypeThread
};
#endif
