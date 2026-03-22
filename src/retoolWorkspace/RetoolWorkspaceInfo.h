#pragma once

#include <json/json.h>
#include <sstream>
#include <string>

struct RetoolWorkspaceInfo
{
    std::string workspaceId;
    std::string email;
    std::string password;
    std::string mailProvider;
    std::string mailAccountId;
    std::string baseUrl;
    std::string subdomain;
    std::string accessToken;
    std::string xsrfToken;
    std::string extraCookiesJson;
    std::string openaiResourceUuid;
    std::string openaiResourceName;
    std::string anthropicResourceUuid;
    std::string anthropicResourceName;
    std::string workflowId;
    std::string workflowApiKey;
    std::string agentId;
    std::string status = "provisioning";
    std::string verifyStatus = "unknown";
    std::string lastVerifyAt;
    std::string lastUsedAt;
    int inUseCount = 0;
    std::string notesJson;
    std::string createdAt;
    std::string updatedAt;

    static RetoolWorkspaceInfo fromJson(const Json::Value& value)
    {
        auto getString = [&value](const char* camel, const char* snake = nullptr, const std::string& defaultValue = "") {
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
        if (value.isMember("extraCookies")) {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            info.extraCookiesJson = Json::writeString(builder, value["extraCookies"]);
        } else if (value.isMember("extra_cookies")) {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            info.extraCookiesJson = Json::writeString(builder, value["extra_cookies"]);
        } else {
            info.extraCookiesJson = getString("extraCookiesJson", "extra_cookies_json", "{}");
        }
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
        if (value.isMember("notes")) {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            info.notesJson = Json::writeString(builder, value["notes"]);
        } else {
            info.notesJson = getString("notesJson", "notes_json", "{}");
        }
        info.createdAt = getString("createdAt", "created_at");
        info.updatedAt = getString("updatedAt", "updated_at");
        return info;
    }

    Json::Value toJson(bool includeSecrets = false) const
    {
        Json::Value value(Json::objectValue);
        value["workspaceId"] = workspaceId;
        value["email"] = email;
        value["mailProvider"] = mailProvider;
        value["mailAccountId"] = mailAccountId;
        value["baseUrl"] = baseUrl;
        value["subdomain"] = subdomain;
        value["openaiResourceUuid"] = openaiResourceUuid;
        value["openaiResourceName"] = openaiResourceName;
        value["anthropicResourceUuid"] = anthropicResourceUuid;
        value["anthropicResourceName"] = anthropicResourceName;
        value["workflowId"] = workflowId;
        value["workflowApiKey"] = workflowApiKey;
        value["agentId"] = agentId;
        value["status"] = status;
        value["verifyStatus"] = verifyStatus;
        value["lastVerifyAt"] = lastVerifyAt;
        value["lastUsedAt"] = lastUsedAt;
        value["inUseCount"] = inUseCount;
        value["createdAt"] = createdAt;
        value["updatedAt"] = updatedAt;

        Json::CharReaderBuilder reader;
        Json::Value extra(Json::objectValue);
        std::string errs;
        if (!extraCookiesJson.empty())
        {
            std::istringstream in(extraCookiesJson);
            if (Json::parseFromStream(reader, in, &extra, &errs) && extra.isObject())
            {
                value["extraCookies"] = extra;
            }
        }

        Json::Value notes(Json::objectValue);
        if (!notesJson.empty())
        {
            std::istringstream in(notesJson);
            if (Json::parseFromStream(reader, in, &notes, &errs) && notes.isObject())
            {
                value["notes"] = notes;
            }
        }

        if (includeSecrets)
        {
            value["password"] = password;
            value["accessToken"] = accessToken;
            value["xsrfToken"] = xsrfToken;
        }
        return value;
    }
};
