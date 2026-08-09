#ifndef DOMAIN_MODEL_CHANNEL_INFO_H
#define DOMAIN_MODEL_CHANNEL_INFO_H

#include <string>

using std::string;

// 渠道信息领域模型（R4 试点 B）。
// 原定义在 dbManager/channel/channelDbManager.h 内。端口 IChannelStore 的签名要用到它，
// 若留在 dbManager 头里，domain/port 就得反向 include dbManager —— 那会污染中立层，
// 并被第三道门禁（layer boundary, rc=3）直接拦下。故先行搬迁。
//
// 结构体名保持 Channelinfo_st 不变：改名会波及 33 处引用，与本次目标无关，属额外风险。
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
    int accountRetentionDays;  // 渠道账号保留天数，0 表示不启用
    bool supportsToolCalls;  // 是否支持函数调用/工具调用
    
    Channelinfo_st() : id(0), channelStatus(true), maxConcurrent(10), timeout(30), priority(0), accountCount(0), accountRetentionDays(0), supportsToolCalls(false) {}
    
    Channelinfo_st(int id, string channelName, string channelType, string channelUrl,
                   string channelKey, bool channelStatus, int maxConcurrent, int timeout,
                   int priority, string description, string createTime, string updateTime,
                   int accountCount = 0, int accountRetentionDays = 0, bool supportsToolCalls = false)
        : id(id), channelName(channelName), channelType(channelType), channelUrl(channelUrl),
          channelKey(channelKey), channelStatus(channelStatus), maxConcurrent(maxConcurrent),
          timeout(timeout), priority(priority), description(description),
          createTime(createTime), updateTime(updateTime), accountCount(accountCount),
          accountRetentionDays(accountRetentionDays),
          supportsToolCalls(supportsToolCalls) {}
    
    Channelinfo_st(string channelName, string channelType, string channelUrl,
                   string channelKey, bool channelStatus, int maxConcurrent,
                   int timeout, int priority, string description, int accountCount = 0,
                   int accountRetentionDays = 0, bool supportsToolCalls = false)
        : id(0), channelName(channelName), channelType(channelType), channelUrl(channelUrl),
          channelKey(channelKey), channelStatus(channelStatus), maxConcurrent(maxConcurrent),
          timeout(timeout), priority(priority), description(description),
          createTime(""), updateTime(""), accountCount(accountCount),
          accountRetentionDays(accountRetentionDays),
          supportsToolCalls(supportsToolCalls) {}

};

#endif
