#pragma once

#include <domain/port/IAccountAdminCommands.h>
#include <domain/port/IAccountAdminUseCase.h>
#include <domain/port/IAccountBackupStore.h>
#include <domain/port/IAccountStore.h>
#include <domain/port/IChannelCatalog.h>

class AccountAdminUseCase final : public IAccountAdminUseCase
{
  public:
    AccountAdminUseCase(IAccountCatalog* catalog,
                        IAccountAdminCommands* commands,
                        IAccountStore* store,
                        IAccountBackupStore* backups,
                        IChannelCatalog* channels,
                        IBackgroundExecutor* executor);

    AccountMap listAccounts() override;
    std::list<Accountinfo_st> listBackupAccounts() override;
    std::list<Accountinfo_st> listStoredAccounts() override;
    std::optional<bool> channelEnabled(const std::string& apiName) const override;
    bool stageAdd(Accountinfo_st account) override;
    bool stageUpdate(Accountinfo_st account) override;
    bool isRegistering(const std::string& userName) override;
    bool stageDelete(const std::string& apiName, const std::string& userName) override;
    TaskSubmitResult persistAdds(std::list<Accountinfo_st> accounts) override;
    TaskSubmitResult persistUpdates(std::list<Accountinfo_st> accounts) override;
    TaskSubmitResult persistDeletes(std::list<Accountinfo_st> accounts) override;
    TaskSubmitResult refreshAccounts() override;
    TaskSubmitResult autoRegister(std::string apiName, int count) override;
    AccountAutomationSettings automationSettings() const override;
    bool updateAutomationSettings(
        const AccountAutomationSettings& settings, std::string* errorMessage) override;

  private:
    IAccountCatalog* catalog_;
    IAccountAdminCommands* commands_;
    IAccountStore* store_;
    IAccountBackupStore* backups_;
    IChannelCatalog* channels_;
    IBackgroundExecutor* executor_;
};
