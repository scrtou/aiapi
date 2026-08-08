#ifndef IRETOOL_WORKSPACE_STORE_H
#define IRETOOL_WORKSPACE_STORE_H

#include <domain/model/RetoolWorkspaceInfo.h>
#include <optional>
#include <string>
#include <vector>

// RetoolWorkspace 持久化端口（R4 依赖倒置试点）。
// 上层 retoolWorkspace 模块只依赖本接口，不再依赖 dbManager 具体实现。
// 方法签名逐字取自 RetoolWorkspaceDbManager，因此既有调用表达式无需改写。
class IRetoolWorkspaceStore
{
  public:
    virtual ~IRetoolWorkspaceStore() = default;

    virtual bool ensureTable(std::string* errorMessage = nullptr) = 0;
    virtual bool upsertWorkspace(const RetoolWorkspaceInfo& info, std::string* errorMessage = nullptr) = 0;
    virtual bool deleteWorkspace(const std::string& workspaceId, std::string* errorMessage = nullptr) = 0;
    virtual std::optional<RetoolWorkspaceInfo> getWorkspace(const std::string& workspaceId,
                                                            std::string* errorMessage = nullptr) = 0;
    virtual std::vector<RetoolWorkspaceInfo> listWorkspaces(std::string* errorMessage = nullptr) = 0;
    virtual bool updateWorkspaceStatus(const std::string& workspaceId,
                                       const std::string& status,
                                       const std::string& verifyStatus,
                                       std::string* errorMessage = nullptr) = 0;
    virtual bool updateWorkspaceUsage(const std::string& workspaceId,
                                      int inUseCount,
                                      bool touchLastUsedAt,
                                      std::string* errorMessage = nullptr) = 0;
};

#endif
