#ifndef ACCOUNT_MANAGER_RETOOL_PROVISION_HEALTH_H
#define ACCOUNT_MANAGER_RETOOL_PROVISION_HEALTH_H

// Retool provision health: consecutive-failure counter + cooldown window.
// Extracted from accountManager.cpp (original lines 31-43 and 114-178).
// Dependency analysis showed the cluster is closed: it calls no other helper
// in that file, and no other helper calls into it.
// It previously lived in an anonymous namespace (internal linkage) and now lives
// in a named namespace (external linkage) -- a deliberate semantic change.
//
// This unit deliberately does NOT include anything from dbManager. It takes an
// IKeyValueConfigStore& (domain/port), exactly like retoolWorkspace takes
// IRetoolWorkspaceStore. The concrete ConfigDbManager adapter lives in
// accountManager.cpp, which is already on the db-include ratchet list, so the
// extraction adds no new direct dbManager edge.

#include <domain/port/IKeyValueConfigStore.h>
#include <string>

namespace retoolProvision {

struct RetoolProvisionHealth
{
    int consecutiveFailures = 0;
    std::string lastFailureAt;
    std::string lastFailureReason;
    std::string cooldownUntil;
};

RetoolProvisionHealth loadRetoolProvisionHealth(IKeyValueConfigStore& store,
                                                std::string* errorMessage = nullptr);
bool persistRetoolProvisionHealth(IKeyValueConfigStore& store,
                                  const RetoolProvisionHealth& state,
                                  std::string* errorMessage = nullptr);
bool isRetoolProvisionCoolingDown(const RetoolProvisionHealth& state);
void markRetoolProvisionSuccess(IKeyValueConfigStore& store);
void markRetoolProvisionFailure(IKeyValueConfigStore& store, const std::string& reason);

}  // namespace retoolProvision

#endif  // ACCOUNT_MANAGER_RETOOL_PROVISION_HEALTH_H
