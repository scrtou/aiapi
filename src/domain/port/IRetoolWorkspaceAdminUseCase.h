#pragma once

#include <domain/port/IRetoolWorkspaceUseCase.h>

#include <string>

namespace workspace {

/**
 * Admin transport facade for workspace workflows.
 *
 * Provider execution deliberately keeps using IRetoolWorkspaceUseCase.  The
 * admin endpoint additionally owns provisioning, so it receives this broader
 * controller-facing use case rather than two unrelated collaborators.
 */
class IRetoolWorkspaceAdminUseCase : public IRetoolWorkspaceUseCase
{
  public:
    ~IRetoolWorkspaceAdminUseCase() override = default;

    virtual RetoolWorkspaceInfo provision(const std::string& requestJson) = 0;
};

}  // namespace workspace
