#ifndef CHAYNSAPI_H
#define CHAYNSAPI_H

#include <infrastructure/provider/chayns/ChaynsClock.h>
#include <infrastructure/provider/chayns/ChaynsHttpTransport.h>
#include <infrastructure/provider/chayns/ChaynsMessageCorrelation.h>
#include <infrastructure/provider/chayns/ChaynsModelCatalog.h>
#include <infrastructure/provider/chayns/ChaynsPollingPolicy.h>
#include <domain/port/IAccountSelector.h>
#include <domain/port/IProviderModelCatalog.h>
#include <domain/port/IProviderThreadContext.h>
#include <infrastructure/provider/ProviderBase.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

constexpr int BASE_DELAY = 100;  // 轮询重试间隔（毫秒）
constexpr int CONSECUTIVE_FAILS_BEFORE_SWITCH = 3;  // 连续失败n次后换账号
constexpr int MAX_UPSTREAM_RETRIES = 4;  // 上游最大总重试次数（外层循环，每次创建新线程或换账号）

class chaynsThreadDbManager;

/**
 * Chayns P6 provider slice.
 *
 * The provider owns only Chayns protocol state (model cache, upstream thread
 * context and account serialization).  It receives a value request and a
 * read-only call context, never a legacy session aggregate.
 */
class chaynsapi final : public provider::ProviderBase,
                        public provider::IProviderModelCatalog,
                        public provider::IProviderThreadContext
{
  public:
    chaynsapi(IAccountSelector& accountSelector,
              std::shared_ptr<chayns::IChaynsHttpTransport> transport,
              std::shared_ptr<chayns::IChaynsClock> clock,
              std::shared_ptr<chaynsThreadDbManager> threadLedger = nullptr,
              FailureObserver failureObserver = {});
    ~chaynsapi() override = default;

    /** Composition-root lifecycle entry; startup failures remain explicit. */
    platform::Result<void> initialize();

    provider::ProviderCapabilities capabilities() const noexcept override;
    ProviderModelCatalog getModels() override;

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
    struct ThreadContext {
        std::string threadId;
        std::string userAuthorId;
        std::string agentAuthorId;
        std::string accountUserName;
        std::string modelId;
        std::string accountType;
        int threadTypeId = 8;
        std::int64_t workspaceUacId = 0;
        std::string origin;
        std::string referer;
        std::string lastRequestMessageId;
        std::string lastRequestCreationTime;
        std::string lastAssistantMessageId;
    };

    [[nodiscard]] bool loadModels(
        bool forceRefresh = false,
        const provider::ProviderCallContext* context = nullptr);
    [[nodiscard]] bool findModel(
        const std::string& modelName,
        chayns::ModelDescriptor& model) const;

    [[nodiscard]] std::string uploadImageToService(
        const ImageInfo& image,
        const std::string& personId,
        const std::string& authToken,
        const std::string& accountUserName,
        const std::string& origin,
        const std::string& referer,
        const provider::ProviderCallContext& context);

    [[nodiscard]] chayns::HttpResult sendWithinContext(
        const provider::ProviderCallContext& context,
        const std::string& baseUrl,
        const drogon::HttpRequestPtr& request,
        double maximumTimeoutSeconds);

    [[nodiscard]] std::shared_ptr<std::mutex> accountExecutionGate(
        const std::string& accountUserName);

    IAccountSelector& m_accountSelector;
    chayns::ModelCatalog m_modelCatalog;
    mutable std::shared_mutex m_modelCatalogMutex;
    std::mutex m_modelRefreshMutex;
    std::chrono::steady_clock::time_point m_modelsLoadedAt{};
    std::chrono::steady_clock::time_point m_lastModelRefreshAttempt{};

    std::shared_ptr<chayns::IChaynsHttpTransport> m_transport;
    std::shared_ptr<chayns::IChaynsClock> m_clock;
    // Context-owned ledger; null is an explicit memory-only/degraded
    // configuration, never a fallback lookup of a global DB manager.
    std::shared_ptr<chaynsThreadDbManager> m_threadLedger;

    std::map<std::string, ThreadContext> m_threadMap;
    std::mutex m_threadMapMutex;

    // A provider instance is runtime-owned. Account serialization therefore
    // belongs to it rather than a process-global map or manager singleton.
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> m_accountGates;
    std::mutex m_accountGatesMutex;

    // 上游错误文本列表，从配置 custom_config.upstream_error_texts 加载
    std::vector<std::string> m_upstreamErrorTexts;
};

#endif
