#ifndef ACCOUNT_MANAGER_H
#define ACCOUNT_MANAGER_H
#include <string>
#include <memory>
#include <queue>
#include <vector>
#include <map>
#include <list>
#include <APIinterface.h>
using namespace std;
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
    Accountinfo_st(){}
    Accountinfo_st(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId,string personId)
    {
        this->apiName = apiName;
        this->userName = userName;
        this->passwd = passwd;
        this->authToken = authToken;
        this->useCount = useCount;
        this->tokenStatus = tokenStatus;
        this->accountStatus = accountStatus;
        this->userTobitId = userTobitId;
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
   // static AccountManager* instance;
    map<string,shared_ptr<priority_queue<shared_ptr<Accountinfo_st>,vector<shared_ptr<Accountinfo_st>>,AccountCompare>>> accountPoolMap;
    list<shared_ptr<Accountinfo_st>> accountList;
     // 使用 std::function 来定义指向成员函数的指针
    map<string, void (AccountManager::*)(shared_ptr<Accountinfo_st>)> updateTokenMap = {
        {"chaynsapi", &AccountManager::updateChaynsToken}
    };

    map<string, bool (AccountManager::*)(string)> checkTokenMap = {
        {"chaynsapi", &AccountManager::checkChaynsToken}
    };
    //list<string> apiNameList;
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

    void addAccount(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId,string personId);
    void getAccount(string apiName,shared_ptr<Accountinfo_st>& account);
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
    bool addAccount(struct Accountinfo_st accountinfo);
    bool updateAccount(struct Accountinfo_st accountinfo);
    bool deleteAccount(struct Accountinfo_st accountinfo);
    list<Accountinfo_st> getAccountDBList();
    list<shared_ptr<Accountinfo_st>> getAccountList();
    bool isTableExist(string tableName);
};
#endif