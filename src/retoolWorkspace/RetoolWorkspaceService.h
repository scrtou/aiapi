#pragma once

#include <functional>
#include <json/json.h>
#include <optional>
#include <domain/model/RetoolWorkspaceInfo.h>
#include <string>
#include <memory>
#include <domain/port/IRetoolWorkspaceUseCase.h>
#include <domain/port/IRetoolWorkspaceStore.h>

class RetoolWorkspaceService : public workspace::IRetoolWorkspaceProvisioner
{
  public:
    // The provisioner retains the injected port so asynchronous callers never
    // borrow a store whose ownership is outside the runtime graph.
    explicit RetoolWorkspaceService(std::shared_ptr<IRetoolWorkspaceStore> workspaceStore);
    RetoolWorkspaceService(const RetoolWorkspaceService&) = delete;
    RetoolWorkspaceService& operator=(const RetoolWorkspaceService&) = delete;

    RetoolWorkspaceInfo provisionWorkspace(const Json::Value& requestBody, std::string* errorMessage = nullptr);
    RetoolWorkspaceInfo provision(const std::string& requestJson) override;

  private:
    std::string orchestratorBaseUrl() const;
    std::string orchestratorApiKey() const;
    std::shared_ptr<IRetoolWorkspaceStore> workspaceStore_;
};
