#ifndef CHANNEL_DBMANAGER_H
#define CHANNEL_DBMANAGER_H

#include <drogon/drogon.h>
#include <string>
#include <list>
#include <memory>
#include <dbManager/account/accountDbManager.h>  // 引入 DbType 枚举
using namespace drogon;
using namespace std;

struct Channelinfo_st
{
    int id;
    string channelName;
    string channelType;
    string channelUrl;
    string channelKey;
    bool channelStatus;
    int maxConcurrent;
    int timeout;
    int priority;
    string description;
    string createTime;
    string updateTime;
    int accountCount;  // 渠道规定的账号数量
    bool supportsToolCalls;  // 是否支持函数调用/工具调用
    
    Channelinfo_st() : id(0), channelStatus(true), maxConcurrent(10), timeout(30), priority(0), accountCount(0), supportsToolCalls(false) {}
    
    Channelinfo_st(int id, string channelName, string channelType, string channelUrl,
                   string channelKey, bool channelStatus, int maxConcurrent, int timeout,
                   int priority, string description, string createTime, string updateTime,
                   int accountCount = 0, bool supportsToolCalls = false)
        : id(id), channelName(channelName), channelType(channelType), channelUrl(channelUrl),
          channelKey(channelKey), channelStatus(channelStatus), maxConcurrent(maxConcurrent),
          timeout(timeout), priority(priority), description(description),
          createTime(createTime), updateTime(updateTime), accountCount(accountCount),
          supportsToolCalls(supportsToolCalls) {}
    
    Channelinfo_st(string channelName, string channelType, string channelUrl,
                   string channelKey, bool channelStatus, int maxConcurrent,
                   int timeout, int priority, string description, int accountCount = 0,
                   bool supportsToolCalls = false)
        : id(0), channelName(channelName), channelType(channelType), channelUrl(channelUrl),
          channelKey(channelKey), channelStatus(channelStatus), maxConcurrent(maxConcurrent),
          timeout(timeout), priority(priority), description(description),
          createTime(""), updateTime(""), accountCount(accountCount),
          supportsToolCalls(supportsToolCalls) {}
};

class ChannelDbManager
{
public:
    static shared_ptr<ChannelDbManager> getInstance()
    {
        static shared_ptr<ChannelDbManager> instance;
        if(instance == nullptr)
        {
            instance = make_shared<ChannelDbManager>();
            instance->dbClient = app().getDbClient("aichatpg");
            instance->detectDbType();
        }
        return instance;
    }
    
    void init();
    bool addChannel(struct Channelinfo_st channelinfo);
    bool updateChannel(struct Channelinfo_st channelinfo);
    bool deleteChannel(int channelId);
    bool getChannel(string channelName, struct Channelinfo_st& channelinfo);
    list<Channelinfo_st> getChannelList();
    bool isTableExist();
    void createTable();
    void checkAndUpgradeTable();
    bool updateChannelStatus(string channelName, bool status);
    DbType getDbType() const { return dbType; }
    
private:
    void detectDbType();
    shared_ptr<drogon::orm::DbClient> dbClient;
    DbType dbType = DbType::PostgreSQL;  // 默认为 PostgreSQL
};

#endif