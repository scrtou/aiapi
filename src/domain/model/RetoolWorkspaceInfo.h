#pragma once

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

};
