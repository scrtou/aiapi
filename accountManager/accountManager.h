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
    bool tokenStatus;
    bool accountStatus;
    int userTobitId;
    Accountinfo_st(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId)
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

class AccountManager
{
    private:
   // static AccountManager* instance;
    map<string,shared_ptr<priority_queue<shared_ptr<Accountinfo_st>,vector<shared_ptr<Accountinfo_st>>,AccountCompare>>> accountPoolMap;
    list<shared_ptr<Accountinfo_st>> accountList;
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
    void addAccount(string apiName,string userName,string passwd,string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId);
    void getAccount(string apiName,shared_ptr<Accountinfo_st>& account);
    void checkAccount();
    void checkToken();
    void registerAPIinterface(string apiName,shared_ptr<APIinterface> apiInterface);
    void refreshAccountQueue(string apiName);
    void printAccountPoolMap();
};
#endif