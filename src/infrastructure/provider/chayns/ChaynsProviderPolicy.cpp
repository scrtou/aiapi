#include <infrastructure/provider/chayns/ChaynsProviderPolicy.h>

namespace chayns::policy {

RequestRoute requestRouteForAccount(const Accountinfo_st& account)
{
    RequestRoute route;
    route.isPro = account.accountType == "pro";
    if (route.isPro) {
        route.threadTypeId = 9;
        route.workspaceUacId = account.workspaceUacId;
        route.origin = kProOrigin;
        route.referer = kProReferer;
    }
    return route;
}

bool isUsableAccount(const std::shared_ptr<Accountinfo_st>& account,
                     bool requiresPro)
{
    if (!account || !account->tokenStatus || !account->accountStatus ||
        account->status != AccountStatus::ACTIVE || account->authToken.empty()) {
        return false;
    }
    if (requiresPro) {
        return account->accountType == "pro" && account->workspaceUacId > 0;
    }
    return account->accountType == "free";
}

bool postFailureMayHaveBeenAccepted(int statusCode)
{
    return statusCode == 408 || statusCode >= 500;
}

}  // namespace chayns::policy
