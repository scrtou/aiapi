#ifndef CHAYNS_PROVIDER_H
#define CHAYNS_PROVIDER_H

#include <infrastructure/provider/chayns/ChaynsClock.h>
#include <infrastructure/provider/chayns/ChaynsHttpTransport.h>
#include <infrastructure/provider/chayns/ChaynsMessageCorrelation.h>
#include <infrastructure/provider/chayns/ChaynsModelCatalog.h>
#include <infrastructure/provider/chayns/ChaynsProtocolClient.h>
#include <infrastructure/provider/chayns/ChaynsPollingPolicy.h>
#include <infrastructure/provider/chayns/ChaynsPollingLoop.h>
#include <infrastructure/provider/chayns/ChaynsThreadContext.h>
#include <domain/port/IAccountSelector.h>
#include <domain/port/IProviderModelCatalog.h>
#include <domain/port/IProviderThreadContext.h>
#include <infrastructure/provider/ProviderBase.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

constexpr int BASE_DELAY = 100;  // 轮询重试间隔（毫秒）
constexpr int CONSECUTIVE_FAILS_BEFORE_SWITCH = 3;  // 连续失败n次后换账号
constexpr int MAX_UPSTREAM_RETRIES = 4;  // 上游最大总重试次数（外层循环，每次创建新线程或换账号）

struct ChaynsProviderSettings
{
    std::vector<std::string> upstreamErrorTexts;
};

/**
 * Chayns P6 provider slice.
 *
 * The provider owns only Chayns protocol state (model cache, upstream thread
 * context and account serialization).  It receives a value request and a
 * read-only call context, never a legacy session aggregate.
 */
class ChaynsProvider final : public provider::ProviderBase,
                             public provider::IProviderModelCatalog,
                             public provider::IProviderThreadContext
{
  public:
    ChaynsProvider(IAccountSelector& accountSelector,
                   std::shared_ptr<chayns::IChaynsHttpTransport> transport,
                   std::shared_ptr<chayns::IChaynsClock> clock,
                   std::shared_ptr<chayns::IChaynsThreadLedger> threadLedger = nullptr,
                   FailureObserver failureObserver = {},
                   ChaynsProviderSettings settings = {});
    ~ChaynsProvider() override = default;

    /** Composition-root lifecycle entry; startup failures remain explicit. */
    platform::Result<void> initialize();

    provider::ProviderCapabilities capabilities() const noexcept override;
    ProviderModelCatalog getModels() override;
    std::optional<ProviderModelCapabilities> findModelCapabilities(
        const std::string& modelId) const override;

    platform::Result<void> eraseThreadContext(
        const std::string& conversationId) override;
    platform::Result<void> transferThreadContext(
        const std::string& oldId,
        const std::string& newId) override;
    platform::Result<void> deleteUpstreamThread(
        const std::string& accountUserName,
        const std::string& threadId,
        const std::string& origin,
        const std::string& referer) override;

  protected:
    platform::Result<provider::ProviderResponse> doGenerate(
        const provider::ProviderRequest& request,
        provider::ProviderCallContext& context) override;
    std::string_view providerName() const noexcept override { return "chaynsapi"; }

  private:
    [[nodiscard]] bool loadModels(
        bool forceRefresh = false,
        const provider::ProviderCallContext* context = nullptr);
    [[nodiscard]] bool findModel(
        const std::string& modelName,
        chayns::ModelDescriptor& model) const;

    [[nodiscard]] std::shared_ptr<std::mutex> accountExecutionGate(
        const std::string& accountUserName);
    [[nodiscard]] std::optional<platform::Error> sleepWithinContext(
        const provider::ProviderCallContext& context,
        std::chrono::milliseconds duration) const;

    IAccountSelector& m_accountSelector;
    chayns::ModelCatalog m_modelCatalog;
    mutable std::shared_mutex m_modelCatalogMutex;
    std::mutex m_modelRefreshMutex;
    std::chrono::steady_clock::time_point m_modelsLoadedAt{};
    std::chrono::steady_clock::time_point m_lastModelRefreshAttempt{};

    std::shared_ptr<chayns::ChaynsProtocolClient> m_protocolClient;
    std::shared_ptr<chayns::IChaynsClock> m_clock;
    chayns::ChaynsPollingLoop m_pollingLoop;
    chayns::ChaynsThreadContext m_threadContext;

    // A provider instance is runtime-owned. Account serialization therefore
    // belongs to it rather than a process-global map or manager singleton.
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> m_accountGates;
    std::mutex m_accountGatesMutex;

    // 上游错误文本列表，从配置 custom_config.upstream_error_texts 加载
    std::vector<std::string> m_upstreamErrorTexts;
};

#endif  // CHAYNS_PROVIDER_H
