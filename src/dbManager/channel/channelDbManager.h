#ifndef CHANNEL_DBMANAGER_H
#define CHANNEL_DBMANAGER_H

#include <drogon/drogon.h>
#include <string>
#include <list>
#include <memory>
#include <dbManager/account/accountDbManager.h>  // 引入 DbType 枚举
#include <domain/model/ChannelInfo.h>  // Channelinfo_st 已搬迁至 domain/model（R4 试点 B）

using std::list;
using std::make_shared;
using std::shared_ptr;
using std::string;
using drogon::app;


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
