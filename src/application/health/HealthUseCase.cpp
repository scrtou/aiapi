#include <application/health/HealthUseCase.h>

#include <domain/port/IAccountCatalog.h>
#include <domain/port/IAccountStore.h>
#include <domain/port/IProviderRegistry.h>

#include <utility>
HealthUseCase::HealthUseCase(
    std::chrono::steady_clock::time_point processStartTime,
    std::shared_ptr<IAccountStore> databaseProbe,
    IProviderRegistry* providers,
    IAccountCatalog* accounts)
    : processStartTime_(processStartTime),
      databaseProbe_(std::move(databaseProbe)),
      providers_(providers),
      accounts_(accounts)
{
}

std::uint64_t HealthUseCase::uptimeSeconds() const
{
    const auto now = std::chrono::steady_clock::now();
    if (now <= processStartTime_) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - processStartTime_).count());
}

HealthReadiness HealthUseCase::readiness() const
{
    HealthReadiness result;

    try {
        result.database = databaseProbe_ && databaseProbe_->isTableExist();
    } catch (...) {
        result.database = false;
    }

    try {
        result.provider = providers_ &&
                          (providers_->findChatProvider("chaynsapi") != nullptr ||
                           providers_->findProvider("chaynsapi") != nullptr);
    } catch (...) {
        result.provider = false;
    }

    try {
        const auto listed = accounts_ ? accounts_->listAccounts()
                                      : IAccountCatalog::AccountMap{};
        for (const auto& apiAccounts : listed) {
            result.accountCount += apiAccounts.second.size();
        }
        result.account = result.accountCount > 0;
    } catch (...) {
        result.account = false;
        result.accountCount = 0;
    }

    return result;
}
