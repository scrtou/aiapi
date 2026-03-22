#ifndef RETOOLAPI_H
#define RETOOLAPI_H

#include <apipoint/APIinterface.h>
#include <apiManager/ApiFactory.h>
#include <drogon/HttpResponse.h>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>

class retoolapi : public APIinterface
{
  public:
    static void* createApi();

    retoolapi();
    ~retoolapi() override;

    provider::ProviderResult generate(session_st& session) override;
    void checkAlivableTokens() override;
    void checkModels() override;
    Json::Value getModels() override;
    void init() override;
    void afterResponseProcess(session_st& session) override;
    void eraseChatinfoMap(std::string conversationId) override;
    void transferThreadContext(const std::string& oldId, const std::string& newId) override;

  private:
    DEClARE_RUNTIME(retoolapi);

    provider::ProviderResult requestWorkflow(session_st& session);
    provider::ProviderResult requestAgent(session_st& session);

    std::string requireWorkspaceId(const session_st& session) const;
    std::string resolveWorkspaceId(session_st& session, bool requireAgent, std::string* errorMessage) const;
    Json::Value resolveRetoolProviderBinding(const Json::Value& workspaceJson, const std::string& model) const;
    bool populateProviderResources(const std::string& workspaceId, Json::Value& workspaceJson) const;
    Json::Value buildRetoolMeta(const std::string& workspaceId,
                                const std::string& routeType,
                                const std::string& resourceId,
                                const Json::Value& binding,
                                const std::string& model) const;
    std::string buildCookieHeader(const Json::Value& workspaceJson) const;
    Json::Value parseJsonResponse(const drogon::HttpResponsePtr& resp) const;
    provider::ProviderError classifyHttpError(int httpStatus, const std::string& message) const;
    drogon::HttpResponsePtr sendJsonRequest(
        const std::string& baseUrl,
        drogon::HttpMethod method,
        const std::string& path,
        const Json::Value* body,
        const Json::Value& workspaceJson,
        double timeoutSeconds = 30.0) const;

    std::string buildTranscriptPrompt(const session_st& session) const;
    std::string lastUserContent(const session_st& session) const;
    std::string contentToText(const Json::Value& content) const;
    std::string encodeJsonString(const std::string& value) const;
    bool replaceFirstRegex(std::string& input, const std::regex& pattern, const std::string& replacement) const;
    Json::Value buildAnthropicWorkflowTemplate(const Json::Value& destinationWorkflow,
                                               const Json::Value& workspaceJson,
                                               const std::string& prompt,
                                               const std::string& model) const;
    Json::Value patchWorkflowTemplate(const Json::Value& workflow, const Json::Value& workspaceJson, const std::string& prompt, const std::string& model) const;
    Json::Value patchAgentTemplate(const Json::Value& workflow, const Json::Value& workspaceJson, const std::string& model) const;

    Json::Value modelListOpenAiFormat_{Json::objectValue};
    std::mutex threadMutex_;
    std::unordered_map<std::string, std::string> agentThreadMap_;
    std::unordered_map<std::string, std::string> conversationWorkspaceMap_;
};

#endif
