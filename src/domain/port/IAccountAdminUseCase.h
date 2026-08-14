#pragma once

#include <domain/model/AccountData.h>
#include <domain/port/IAccountCatalog.h>
#include <domain/port/IBackgroundExecutor.h>

#include <list>
#include <optional>
#include <string>

class IAccountAdminUseCase
{
  public:
    using AccountMap = IAccountCatalog::AccountMap;
    virtual ~IAccountAdminUseCase() = default;

    virtual AccountMap listAccounts() = 0;
    virtual std::list<Accountinfo_st> listBackupAccounts() = 0;
    virtual std::list<Accountinfo_st> listStoredAccounts() = 0;
    virtual std::optional<bool> channelEnabled(const std::string& apiName) const = 0;

    virtual bool stageAdd(Accountinfo_st account) = 0;
    virtual bool stageUpdate(Accountinfo_st account) = 0;
    virtual bool isRegistering(const std::string& userName) = 0;
    virtual bool stageDelete(const std::string& apiName, const std::string& userName) = 0;

    [[nodiscard]] virtual TaskSubmitResult persistAdds(std::list<Accountinfo_st> accounts) = 0;
    [[nodiscard]] virtual TaskSubmitResult persistUpdates(std::list<Accountinfo_st> accounts) = 0;
    [[nodiscard]] virtual TaskSubmitResult persistDeletes(std::list<Accountinfo_st> accounts) = 0;
    [[nodiscard]] virtual TaskSubmitResult refreshAccounts() = 0;
    [[nodiscard]] virtual TaskSubmitResult autoRegister(
        std::string apiName, int count) = 0;

    virtual AccountAutomationSettings automationSettings() const = 0;
    virtual bool updateAutomationSettings(
        const AccountAutomationSettings& settings, std::string* errorMessage) = 0;
};
