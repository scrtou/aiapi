#ifndef CHANNEL_DBMANAGER_H
#define CHANNEL_DBMANAGER_H

#include <drogon/drogon.h>
#include <string>
#include <list>
#include <memory>
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
    
    Channelinfo_st() : id(0), channelStatus(true), maxConcurrent(10), timeout(30), priority(0) {}
    
    Channelinfo_st(int id, string channelName, string channelType, string channelUrl,
                   string channelKey, bool channelStatus, int maxConcurrent, int timeout,
                   int priority, string description, string createTime, string updateTime)
        : id(id), channelName(channelName), channelType(channelType), channelUrl(channelUrl),
          channelKey(channelKey), channelStatus(channelStatus), maxConcurrent(maxConcurrent),
          timeout(timeout), priority(priority), description(description),
          createTime(createTime), updateTime(updateTime) {}
    
    Channelinfo_st(string channelName, string channelType, string channelUrl,
                   string channelKey, bool channelStatus, int maxConcurrent,
                   int timeout, int priority, string description)
        : id(0), channelName(channelName), channelType(channelType), channelUrl(channelUrl),
          channelKey(channelKey), channelStatus(channelStatus), maxConcurrent(maxConcurrent),
          timeout(timeout), priority(priority), description(description),
          createTime(""), updateTime("") {}
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
    bool updateChannelStatus(string channelName, bool status);
    
private:
    shared_ptr<drogon::orm::DbClient> dbClient;
};

#endif