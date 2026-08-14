#pragma once

#include <domain/model/ChannelInfo.h>
#include <domain/model/RetoolWorkspaceInfo.h>

#include <optional>
#include <cstddef>
#include <string>
#include <vector>

namespace workspace {

struct PoolStatus
{
    std::size_t total = 0;
    int idle = 0;
    int inUse = 0;
    int disabled = 0;
    std::string latestUsedAt;
    std::optional<Channelinfo_st> channel;
    int consecutiveFailures = 0;
    std::string lastFailureAt;
    std::string lastFailureReason;
    std::string cooldownUntil;
};

class IRetoolWorkspaceUseCase
{
  public:
    virtual ~IRetoolWorkspaceUseCase() = default;
    virtual bool upsert(RetoolWorkspaceInfo& info, std::string* error) = 0;
    virtual std::optional<RetoolWorkspaceInfo> get(
        const std::string& workspaceId, std::string* error) = 0;
    virtual bool hasExecutionContext(const RetoolWorkspaceInfo& workspace) const = 0;
    virtual std::vector<RetoolWorkspaceInfo> list(std::string* error = nullptr) = 0;
    virtual bool markUsageStarted(const std::string& workspaceId,
                                  std::string* error) = 0;
    virtual bool markUsageFinished(const std::string& workspaceId,
                                   std::string* error) = 0;
    virtual bool disable(const std::string& workspaceId, std::string* error) = 0;
    virtual bool enable(const std::string& workspaceId, std::string* nextStatus,
                        std::string* verifyStatus, std::string* error) = 0;
    virtual bool remove(const std::string& workspaceId, std::string* error) = 0;
    virtual bool verify(const std::string& workspaceId, bool* ready,
                        std::string* verifyStatus, RetoolWorkspaceInfo* workspace,
                        std::string* error) = 0;
    virtual PoolStatus poolStatus() = 0;
};

class IRetoolWorkspaceProvisioner
{
  public:
    virtual ~IRetoolWorkspaceProvisioner() = default;
    virtual RetoolWorkspaceInfo provision(const std::string& requestJson) = 0;
};

}  // namespace workspace
