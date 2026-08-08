#ifndef DOMAIN_MODEL_ACCOUNT_DATA_H
#define DOMAIN_MODEL_ACCOUNT_DATA_H

/**
 * @brief 账号的纯数据表示（Accountinfo_st 及其配套值类型）
 *
 * 从 accountManager/accountManager.h 抽出。原文件把纯数据 Accountinfo_st 与
 * 带线程/DB/业务编排的 AccountManager 混装，导致 dbManager 的两个 DbManager
 * 头（只需要这个结构体做参数和返回值）被迫反向依赖整个 accountManager，
 * 形成 accountManager <--> dbManager 双向边。
 *
 * 本文件不含行为逻辑，不依赖 accountManager 的任何头。
 */

#include <cstdint>
#include <memory>
#include <string>
#include <json/json.h>

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

    // 将请求/数据库中的 JSON 字段解析为统一账号结构（仅保留 camelCase 新命名）。
    static Accountinfo_st fromJson(const Json::Value& value)
    {
        Accountinfo_st result;
        result.apiName = value.get("apiName", "").asString();
        result.userName = value.get("userName", "").asString();
        result.passwd = value.get("password", "").asString();
        result.authToken = value.get("authToken", "").asString();
        result.useCount = value.get("useCount", 0).asInt();
        result.tokenStatus = value.get("tokenStatus", false).asBool();
        result.accountStatus = value.get("accountStatus", false).asBool();
        result.userTobitId = value.get("userTobitId", 0).asInt();
        result.personId = value.get("personId", "").asString();
        result.createTime = value.get("createTime", "").asString();
        result.accountType = value.get("accountType", "free").asString();
        result.status = value.get("status", "active").asString();
        result.workspaceUacId = value.get("workspaceUacId", Json::Int64(0)).asInt64();
        if (result.workspaceUacId < 0) {
            result.workspaceUacId = 0;
        }
        return result;
    }

    // 将账号结构导出为统一 JSON（仅保留 camelCase 新命名）。
    Json::Value toJson() const
    {
        Json::Value value;
        value["apiName"] = apiName;
        value["userName"] = userName;
        value["password"] = passwd;
        value["authToken"] = authToken;
        value["useCount"] = useCount;
        value["tokenStatus"] = tokenStatus;
        value["accountStatus"] = accountStatus;
        value["userTobitId"] = userTobitId;
        value["personId"] = personId;
        value["createTime"] = createTime;
        value["accountType"] = accountType;
        value["status"] = status;
        value["workspaceUacId"] = Json::Int64(workspaceUacId);
        return value;
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
