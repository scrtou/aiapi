#ifndef APIINTERFACE_H
#define APIINTERFACE_H

#include <domain/model/ProviderModelCatalog.h>
#include <domain/model/ProviderResult.h>

#include <map>
#include <string>

struct session_st;

struct modelInfo
{
    std::string modelName;
    bool status = true;
};

// Transitional wide port.  P6 splits this into chat, catalog, lifecycle and
// thread-context ports; until then it remains dependency-neutral.
class APIinterface
{
  public:
    virtual ~APIinterface() = default;
    virtual provider::ProviderResult generate(session_st& session) = 0;
    virtual void checkAlivableTokens() = 0;
    virtual void checkModels() = 0;
    virtual ProviderModelCatalog getModels() = 0;
    virtual void init() = 0;
    virtual void afterResponseProcess(session_st& session) = 0;
    virtual void eraseChatinfoMap(std::string conversationId) = 0;
    virtual void transferThreadContext(const std::string& oldId,
                                       const std::string& newId) = 0;

    std::map<std::string, modelInfo> ModelInfoMap;
};

#endif  // APIINTERFACE_H
