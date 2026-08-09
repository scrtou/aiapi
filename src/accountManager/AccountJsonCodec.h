#ifndef ACCOUNT_JSON_CODEC_H
#define ACCOUNT_JSON_CODEC_H

#include <domain/model/AccountData.h>
#include <json/json.h>

namespace accountcodec {

inline Accountinfo_st fromJson(const Json::Value& value)
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
    if (result.workspaceUacId < 0) result.workspaceUacId = 0;
    return result;
}

inline Json::Value toJson(const Accountinfo_st& account, bool includeSecrets = false)
{
    Json::Value value(Json::objectValue);
    value["apiName"] = account.apiName;
    value["userName"] = account.userName;
    value["useCount"] = account.useCount;
    value["tokenStatus"] = account.tokenStatus;
    value["accountStatus"] = account.accountStatus;
    value["userTobitId"] = account.userTobitId;
    value["personId"] = account.personId;
    value["createTime"] = account.createTime;
    value["accountType"] = account.accountType;
    value["status"] = account.status;
    value["workspaceUacId"] = Json::Int64(account.workspaceUacId);
    if (includeSecrets)
    {
        value["password"] = account.passwd;
        value["authToken"] = account.authToken;
    }
    return value;
}

}  // namespace accountcodec

#endif  // ACCOUNT_JSON_CODEC_H
