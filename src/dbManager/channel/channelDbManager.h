#ifndef CHANNEL_DBMANAGER_H
#define CHANNEL_DBMANAGER_H

#include <drogon/drogon.h>
#include <string>
#include <list>
#include <memory>
#include <dbManager/DbType.h>
#include <domain/port/IChannelStore.h>
#include <domain/model/ChannelInfo.h>  // Channelinfo_st 已搬迁至 domain/model（R4 试点 B）

using std::list;
using std::shared_ptr;
using std::string;


class ChannelDbManager : public IChannelStore
{
public:
    // The composition root owns this concrete store.  Construction itself
    // must not reach into the process-global Drogon app.
    ChannelDbManager() = default;
    ChannelDbManager(const ChannelDbManager&) = delete;
    ChannelDbManager& operator=(const ChannelDbManager&) = delete;

    /// Bind the configured DB client before ChannelManager performs table work.
    bool initialize(std::string* errorMessage = nullptr);
    
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
    bool initialized_ = false;
};

#endif
