#pragma once

#include <domain/model/AccountData.h>
#include <domain/port/IAccountSettingsQuery.h>

#include <memory>
#include <string>

class IAccountAdminCommands : public IAccountSettingsQuery
{
  public:
    virtual ~IAccountAdminCommands() = default;
    virtual bool addAccountbyPost(Accountinfo_st account) = 0;
    virtual bool updateAccount(Accountinfo_st account) = 0;
    virtual bool deleteAccountbyPost(std::string apiName, std::string userName) = 0;
    virtual bool isAccountRegisteringByUsername(const std::string& userName) = 0;
    virtual bool deleteUpstreamAccount(const Accountinfo_st& account) = 0;
    virtual void loadAccount() = 0;
    virtual void checkUpdateAccountToken() = 0;
    virtual void updateAccountType(std::shared_ptr<Accountinfo_st> account) = 0;
    virtual void checkChannelAccountCounts() = 0;
    virtual void checkToken() = 0;
    virtual void updateAllAccountTypes() = 0;
    virtual bool autoRegisterAccount(std::string apiName) = 0;
    virtual AccountAutomationSettings getAccountAutomationSettings() const = 0;
    virtual bool updateAccountAutomationSettings(
        const AccountAutomationSettings& settings, bool persistToConfig,
        std::string* errorMessage) = 0;
};
