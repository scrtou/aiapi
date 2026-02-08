#ifndef OPENAI_PROVIDER_H
#define OPENAI_PROVIDER_H

#include <apipoint/APIinterface.h>
#include <apiManager/ApiFactory.h>
#include <mutex>
#include <string>

class OpenAiProvider : public APIinterface {
public:
    static void* createApi();

    OpenAiProvider();
    ~OpenAiProvider() override;

    void postChatMessage(session_st& session);
    provider::ProviderResult generate(session_st& session) override;
    void checkAlivableTokens() override;
    void checkModels() override;
    Json::Value getModels() override;
    void init() override;
    void afterResponseProcess(session_st& session) override;
    void eraseChatinfoMap(std::string conversationId) override;
    void transferThreadContext(const std::string& oldId, const std::string& newId) override;

private:
    DEClARE_RUNTIME(OpenAiProvider);

    provider::ProviderResult requestChatCompletions(session_st& session);
    Json::Value buildChatRequest(const session_st& session) const;

    std::string apiKey_;
    std::string baseUrl_;
    std::string defaultModel_;

    std::mutex modelMutex_;
    Json::Value modelList_{Json::arrayValue};
    Json::Value modelListOpenAiFormat_{Json::arrayValue};
};

#endif
