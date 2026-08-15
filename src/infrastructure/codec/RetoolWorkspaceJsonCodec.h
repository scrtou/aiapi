#ifndef RETOOL_WORKSPACE_JSON_CODEC_H
#define RETOOL_WORKSPACE_JSON_CODEC_H

#include <domain/model/RetoolWorkspaceInfo.h>
#include <json/json.h>

#include <sstream>

namespace retoolworkspacecodec {

inline std::string compactJson(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

inline RetoolWorkspaceInfo fromJson(const Json::Value& value)
{
    auto getString = [&value](const char* camel,
                              const char* snake = nullptr,
                              const std::string& defaultValue = "") {
        if (value.isMember(camel)) return value.get(camel, defaultValue).asString();
        if (snake && value.isMember(snake)) return value.get(snake, defaultValue).asString();
        return defaultValue;
    };

    RetoolWorkspaceInfo info;
    info.workspaceId = getString("workspaceId", "workspace_id");
    info.email = getString("email");
    info.password = getString("password");
    info.mailProvider = getString("mailProvider", "mail_provider");
    info.mailAccountId = getString("mailAccountId", "mail_account_id");
    info.baseUrl = getString("baseUrl", "base_url");
    info.subdomain = getString("subdomain");
    info.accessToken = getString("accessToken", "access_token");
    info.xsrfToken = getString("xsrfToken", "xsrf_token");
    if (value.isMember("extraCookies"))
        info.extraCookiesJson = compactJson(value["extraCookies"]);
    else if (value.isMember("extra_cookies"))
        info.extraCookiesJson = compactJson(value["extra_cookies"]);
    else
        info.extraCookiesJson = getString("extraCookiesJson", "extra_cookies_json", "{}");
    info.openaiResourceUuid = getString("openaiResourceUuid", "openai_resource_uuid");
    info.openaiResourceName = getString("openaiResourceName", "openai_resource_name");
    info.anthropicResourceUuid = getString("anthropicResourceUuid", "anthropic_resource_uuid");
    info.anthropicResourceName = getString("anthropicResourceName", "anthropic_resource_name");
    info.workflowId = getString("workflowId", "workflow_id");
    info.workflowApiKey = getString("workflowApiKey", "workflow_api_key");
    info.agentId = getString("agentId", "agent_id");
    info.status = getString("status", nullptr, info.status);
    info.verifyStatus = getString("verifyStatus", "verify_status", info.verifyStatus);
    info.lastVerifyAt = getString("lastVerifyAt", "last_verify_at");
    info.lastUsedAt = getString("lastUsedAt", "last_used_at");
    info.inUseCount = value.isMember("inUseCount")
        ? value.get("inUseCount", 0).asInt()
        : value.get("in_use_count", 0).asInt();
    info.notesJson = value.isMember("notes")
        ? compactJson(value["notes"])
        : getString("notesJson", "notes_json", "{}");
    info.createdAt = getString("createdAt", "created_at");
    info.updatedAt = getString("updatedAt", "updated_at");
    return info;
}

inline bool parseObject(const std::string& encoded, Json::Value& output)
{
    if (encoded.empty()) return false;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(encoded);
    return Json::parseFromStream(reader, input, &output, &errors) && output.isObject();
}

inline Json::Value toJson(const RetoolWorkspaceInfo& info, bool includeSecrets = false)
{
    Json::Value value(Json::objectValue);
    value["workspaceId"] = info.workspaceId;
    value["email"] = info.email;
    value["mailProvider"] = info.mailProvider;
    value["mailAccountId"] = info.mailAccountId;
    value["baseUrl"] = info.baseUrl;
    value["subdomain"] = info.subdomain;
    value["openaiResourceUuid"] = info.openaiResourceUuid;
    value["openaiResourceName"] = info.openaiResourceName;
    value["anthropicResourceUuid"] = info.anthropicResourceUuid;
    value["anthropicResourceName"] = info.anthropicResourceName;
    value["workflowId"] = info.workflowId;
    value["workflowApiKey"] = info.workflowApiKey;
    value["agentId"] = info.agentId;
    value["status"] = info.status;
    value["verifyStatus"] = info.verifyStatus;
    value["lastVerifyAt"] = info.lastVerifyAt;
    value["lastUsedAt"] = info.lastUsedAt;
    value["inUseCount"] = info.inUseCount;
    value["createdAt"] = info.createdAt;
    value["updatedAt"] = info.updatedAt;

    Json::Value extra(Json::objectValue);
    if (parseObject(info.extraCookiesJson, extra)) value["extraCookies"] = extra;
    Json::Value notes(Json::objectValue);
    if (parseObject(info.notesJson, notes)) value["notes"] = notes;

    if (includeSecrets)
    {
        value["password"] = info.password;
        value["accessToken"] = info.accessToken;
        value["xsrfToken"] = info.xsrfToken;
    }
    return value;
}

}  // namespace retoolworkspacecodec

#endif  // RETOOL_WORKSPACE_JSON_CODEC_H
