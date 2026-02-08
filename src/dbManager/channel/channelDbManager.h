#ifndef CHANNEL_DBMANAGER_H
#define CHANNEL_DBMANAGER_H

#include <drogon/drogon.h>
#include <string>
#include <list>
#include <memory>
#include <dbManager/account/accountDbManager.h>  // 引入 DbType 枚举

using std::list;
using std::make_shared;
using std::shared_ptr;
using std::string;
using drogon::app;

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

    // --- JSON 序列化/反序列化 () ---
    static Channelinfo_st fromJson(const Json::Value& j) {
        Channelinfo_st c;
        c.id                = j.get("id", 0).asInt();
        c.channelName       = j.get("channelname", "").asString();
        c.channelType       = j.get("channeltype", "").asString();
        c.channelUrl        = j.get("channelurl", "").asString();
        c.channelKey        = j.get("channelkey", "").asString();
        c.channelStatus     = j.get("channelstatus", true).asBool();
        c.maxConcurrent     = j.get("maxconcurrent", 10).asInt();
        c.timeout           = j.get("timeout", 30).asInt();
        c.priority          = j.get("priority", 0).asInt();
        c.description       = j.get("description", "").asString();
        c.createTime        = j.get("createtime", "").asString();
        c.updateTime        = j.get("updatetime", "").asString();
        c.accountCount      = j.get("accountcount", 0).asInt();
        c.supportsToolCalls = j.get("supports_tool_calls", false).asBool();
        return c;
    }

    Json::Value toJson() const {
        Json::Value j;
        j["id"]                  = id;
        j["channelname"]         = channelName;
        j["channeltype"]         = channelType;
        j["channelurl"]          = channelUrl;
        j["channelkey"]          = channelKey;
        j["channelstatus"]       = channelStatus;
        j["maxconcurrent"]       = maxConcurrent;
        j["timeout"]             = timeout;
        j["priority"]            = priority;
        j["description"]         = description;
        j["createtime"]          = createTime;
        j["updatetime"]          = updateTime;
        j["accountcount"]        = accountCount;
        j["supports_tool_calls"] = supportsToolCalls;
        return j;
    }
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
    DbType dbType = DbType::PostgreSQL;
};

#endif
