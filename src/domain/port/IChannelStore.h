#ifndef DOMAIN_PORT_ICHANNEL_STORE_H
#define DOMAIN_PORT_ICHANNEL_STORE_H

#include <domain/model/ChannelInfo.h>
#include <list>
#include <string>

// Channel 持久化端口（R4 依赖倒置试点 B）。
// 上层 channelManager 只依赖本接口，不再依赖 dbManager 具体实现。
// 方法签名逐字取自 ChannelDbManager，因此既有调用表达式无需改写。
//
// 刻意不纳入端口的成员：
//   getDbType() —— channelManager 从未调用（取证见步骤 41.4），
//                  且 DbType 是 dbManager 的内部实现细节，纳入会把它拖进 domain。
class IChannelStore
{
  public:
    virtual ~IChannelStore() = default;

    virtual bool addChannel(struct Channelinfo_st channelinfo) = 0;
    virtual bool updateChannel(struct Channelinfo_st channelinfo) = 0;
    virtual bool deleteChannel(int channelId) = 0;
    virtual bool getChannel(std::string channelName, struct Channelinfo_st& channelinfo) = 0;
    virtual std::list<Channelinfo_st> getChannelList() = 0;
    virtual bool isTableExist() = 0;
    virtual void createTable() = 0;
    virtual void checkAndUpgradeTable() = 0;
    virtual bool updateChannelStatus(std::string channelName, bool status) = 0;
};

#endif
