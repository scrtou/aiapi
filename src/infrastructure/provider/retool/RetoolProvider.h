#ifndef RETOOL_PROVIDER_H
#define RETOOL_PROVIDER_H

#include <infrastructure/provider/retool/RetoolClock.h>
#include <infrastructure/provider/retool/RetoolAgentClient.h>
#include <infrastructure/provider/retool/RetoolHttpTransport.h>
#include <infrastructure/provider/retool/RetoolProviderSettings.h>
#include <infrastructure/provider/retool/RetoolWorkspaceContext.h>
#include <infrastructure/provider/retool/RetoolWorkflowClient.h>
#include <domain/port/IChannelCatalog.h>
#include <domain/port/IProviderModelCatalog.h>
#include <domain/port/IProviderThreadContext.h>
#include <domain/port/IRetoolWorkspaceUseCase.h>
#include <infrastructure/provider/ProviderBase.h>
#include <infrastructure/managedAccount/contracts/ManagedAccount.h>

#include <memory>
#include <regex>
#include <string>
#include <string_view>

/**
 * Retool P6 provider slice.
 *
 * Workflow/agent protocol state remains private to the adapter.  It accepts
 * value-only ProviderRequest data and a read-only request control context;
 * neither a legacy session aggregate nor a response-side session mutation can
 * cross this boundary.
 */
class RetoolProvider final : public provider::ProviderBase,
                             public provider::IProviderModelCatalog,
                             public provider::IProviderThreadContext
{
  public:
    RetoolProvider(std::shared_ptr<retool::IRetoolHttpTransport> transport,
                   std::shared_ptr<retool::IRetoolClock> clock,
                   IManagedAccountContextResolver& accounts,
                   workspace::IRetoolWorkspaceUseCase& workspaces,
                   IChannelCatalog& channels,
                   provider::ProviderBase::FailureObserver failureObserver = {},
                   retool::RetoolProviderSettings settings = {});
    ~RetoolProvider() override;

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
    std::string_view providerName() const noexcept override { return "retoolapi"; }

  private:
    platform::Result<provider::ProviderResponse> requestWorkflow(
        const provider::ProviderRequest& request,
        provider::ProviderCallContext& context);
    platform::Result<provider::ProviderResponse> requestAgent(
        const provider::ProviderRequest& request,
        provider::ProviderCallContext& context);

    std::string requireWorkspaceId(const provider::ProviderRequest& request) const;
    std::string resolveWorkspaceId(const provider::ProviderRequest& request,
                                   bool requireAgent,
                                   std::string* errorMessage);
    Json::Value resolveRetoolProviderBinding(const Json::Value& workspaceJson,
                                             const std::string& model) const;
    bool populateProviderResources(const provider::ProviderCallContext& context,
                                   const std::string& workspaceId,
                                   Json::Value& workspaceJson) const;
    provider::ProviderMetadata buildRetoolMeta(const std::string& workspaceId,
                                               const std::string& routeType,
                                               const std::string& resourceId,
                                               const Json::Value& binding,
                                               const std::string& model) const;
    platform::Result<Json::Value> parseJsonResponse(
        const drogon::HttpResponsePtr& resp,
        std::string_view operation) const;
    bool sleepWithinContext(const provider::ProviderCallContext& context,
                            std::chrono::milliseconds duration) const;

    std::string buildTranscriptPrompt(const provider::ProviderRequest& request) const;
    std::string lastUserContent(const provider::ProviderRequest& request) const;
    std::string encodeJsonString(const std::string& value) const;
    bool replaceFirstRegex(std::string& input,
                           const std::regex& pattern,
                           const std::string& replacement) const;
    Json::Value buildAnthropicWorkflowTemplate(
        const provider::ProviderCallContext& context,
        const Json::Value& destinationWorkflow,
        const Json::Value& workspaceJson,
        const std::string& prompt,
        const std::string& model) const;
    Json::Value patchWorkflowTemplate(const Json::Value& workflow,
                                      const Json::Value& workspaceJson,
                                      const std::string& prompt,
                                      const std::string& model) const;
    Json::Value patchAgentTemplate(const Json::Value& workflow,
                                   const Json::Value& workspaceJson,
                                   const std::string& model) const;

    ProviderModelCatalog modelCatalog_;
    std::shared_ptr<retool::IRetoolClock> clock_;
    std::shared_ptr<retool::RetoolWorkflowClient> workflowClient_;
    std::shared_ptr<retool::RetoolAgentClient> agentClient_;
    retool::RetoolWorkspaceContext workspaceContext_;
    IManagedAccountContextResolver* accounts_ = nullptr;
    workspace::IRetoolWorkspaceUseCase* workspaces_ = nullptr;
    IChannelCatalog* channels_ = nullptr;
    retool::RetoolProviderSettings settings_;
};

#endif  // RETOOL_PROVIDER_H
