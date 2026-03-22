#pragma once

#include <functional>
#include <json/json.h>
#include <optional>
#include <retoolWorkspace/RetoolWorkspaceInfo.h>
#include <string>

class RetoolWorkspaceService
{
  public:
    static RetoolWorkspaceService& getInstance()
    {
        static RetoolWorkspaceService instance;
        return instance;
    }

    using ProvisionCallback = std::function<void(const std::optional<RetoolWorkspaceInfo>& workspace, const std::string& errorMessage)>;

    RetoolWorkspaceInfo provisionWorkspace(const Json::Value& requestBody, std::string* errorMessage = nullptr);
    void provisionWorkspaceAsync(const Json::Value& requestBody, ProvisionCallback callback);

  private:
    RetoolWorkspaceService() = default;

    std::string orchestratorBaseUrl() const;
    std::string orchestratorApiKey() const;
};
