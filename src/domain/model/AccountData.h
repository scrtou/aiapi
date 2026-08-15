#ifndef DOMAIN_MODEL_ACCOUNT_DATA_H
#define DOMAIN_MODEL_ACCOUNT_DATA_H

/**
 * @brief 账号的纯数据表示（Accountinfo_st 及其配套值类型）
 *
 * 从 application/account/accountManager.h 抽出。原文件把纯数据 Accountinfo_st 与
 * 带线程/DB/业务编排的 AccountManager 混装，导致 dbManager 的两个 DbManager
 * 头（只需要这个结构体做参数和返回值）被迫反向依赖整个 accountManager，
 * 形成 accountManager <--> dbManager 双向边。
 *
 * 本文件不含行为逻辑，不依赖 accountManager 的任何头。
 */

#include <cstdint>
#include <memory>
#include <string>

// 账号状态常量
namespace AccountStatus {
    const std::string WAITING = "waiting";       // 待注册（已创建占位记录）
    const std::string REGISTERING = "registering"; // 注册中（HTTP请求已发送）
    const std::string ACTIVE = "active";         // 正常激活
    const std::string DISABLED = "disabled";     // 已禁用
}

struct Accountinfo_st
{
    std::string apiName;
    std::string userName;
    std::string passwd;
    std::string authToken;
    int useCount;
    bool tokenStatus=false;
    bool accountStatus=false;
    int userTobitId;
    std::string personId;
    std::string createTime;
    std::string accountType;  // 账号类型: "pro" 或 "free"
    std::string status;       // 账号状态: "pending", "active", "disabled"
    std::int64_t workspaceUacId = 0; // Chayns Pro 账号所属 workspace

    Accountinfo_st(){}
    Accountinfo_st(std::string apiName,std::string userName,std::string passwd,std::string authToken,int useCount,bool tokenStatus,bool accountStatus,int userTobitId,std::string personId,std::string createTime="",std::string accountType="free",std::string status="active",std::int64_t workspaceUacId=0)
    {
        this->apiName = apiName;
        this->userName = userName;
        this->passwd = passwd;
        this->authToken = authToken;
        this->useCount = useCount;
        this->tokenStatus = tokenStatus;
        this->accountStatus = accountStatus;
        this->userTobitId = userTobitId;
        this->personId = personId;
        this->createTime = createTime;
        this->accountType = accountType;
        this->status = status;
        this->workspaceUacId = workspaceUacId;
    }

};

struct AccountCompare
{
    bool operator()(const std::shared_ptr<Accountinfo_st>& a, const std::shared_ptr<Accountinfo_st>& b)
    {
        if(a->tokenStatus!=b->tokenStatus)return b->tokenStatus;
        return a->useCount > b->useCount;
    }
};

struct AccountAutomationSettings
{
    bool autoDeleteEnabled = true;
    int deleteAfterDays = 6;
    bool autoRegisterEnabled = true;
    // Responses API namespace definitions are recursively flattened into Tool Bridge leaves.
    // Enabled by default for backward compatibility.
    bool namespaceToolBridgeEnabled = true;
};

enum class AccountRequirement
{
    AnyValid,
    FreeOnly,
    ProOnly
};
//定义函数指针

#endif  // DOMAIN_MODEL_ACCOUNT_DATA_H
