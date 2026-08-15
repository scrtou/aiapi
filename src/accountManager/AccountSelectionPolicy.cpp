#include <accountManager/AccountSelectionPolicy.h>

namespace account::selection {

bool isPoolMember(const std::shared_ptr<Accountinfo_st>& candidate)
{
    return candidate && candidate->status == AccountStatus::ACTIVE;
}

bool matchesRequirement(const std::shared_ptr<Accountinfo_st>& candidate,
                        const std::string& apiName,
                        AccountRequirement requirement,
                        const std::set<std::string>& excludedUsers)
{
    if (!isPoolMember(candidate) || excludedUsers.count(candidate->userName) > 0 ||
        !candidate->tokenStatus || !candidate->accountStatus || candidate->authToken.empty()) {
        return false;
    }
    // A Pro Chayns credential is only routable after the workspace binding has
    // been persisted.  Keeping this here makes the selector's safety rule
    // independent from the queue traversal implementation.
    if (apiName == "chaynsapi" && candidate->accountType == "pro" &&
        candidate->workspaceUacId <= 0) {
        return false;
    }
    switch (requirement) {
        case AccountRequirement::FreeOnly:
            return candidate->accountType == "free";
        case AccountRequirement::ProOnly:
            return candidate->accountType == "pro";
        case AccountRequirement::AnyValid:
        default:
            return true;
    }
}

const char* requirementName(AccountRequirement requirement)
{
    switch (requirement) {
        case AccountRequirement::FreeOnly:
            return "FreeOnly";
        case AccountRequirement::ProOnly:
            return "ProOnly";
        case AccountRequirement::AnyValid:
        default:
            return "AnyValid";
    }
}

}  // namespace account::selection
