#pragma once

#include <domain/model/AccountData.h>

#include <memory>
#include <set>
#include <string>

/**
 * Read/select side of the account pool used by upstream providers.
 *
 * Account administration deliberately lives on IAccountAdminCommands and
 * IAccountCatalog.  Providers need neither capability: they only need to
 * reuse a named account or lease an eligible one.  Keeping this port narrow
 * avoids making every controller fake implement provider-selection policy.
 */
class IAccountSelector
{
  public:
    virtual ~IAccountSelector() = default;

    virtual bool getEligibleAccount(
        const std::string& apiName,
        std::shared_ptr<Accountinfo_st>& account,
        AccountRequirement requirement,
        const std::set<std::string>& excludedUsers = {}) = 0;

    virtual void getAccountByUserName(
        const std::string& apiName,
        const std::string& userName,
        std::shared_ptr<Accountinfo_st>& account) = 0;
};
