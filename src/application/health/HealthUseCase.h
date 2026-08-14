#pragma once

#include <domain/port/IHealthUseCase.h>

#include <chrono>
#include <memory>

class IAccountCatalog;
class IAccountStore;
class IProviderRegistry;

class HealthUseCase final : public IHealthUseCase
{
  public:
    HealthUseCase(std::chrono::steady_clock::time_point processStartTime,
                  std::shared_ptr<IAccountStore> databaseProbe,
                  IProviderRegistry* providers,
                  IAccountCatalog* accounts);

    std::uint64_t uptimeSeconds() const override;
    HealthReadiness readiness() const override;

  private:
    std::chrono::steady_clock::time_point processStartTime_;
    std::shared_ptr<IAccountStore> databaseProbe_;
    IProviderRegistry* providers_;
    IAccountCatalog* accounts_;
};
