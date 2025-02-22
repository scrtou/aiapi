#include "ApiFactory.h"
#include <Apicomn.h>
ApiFactory& ApiFactory::getInstance()
{
    static ApiFactory instance;
    return instance;
}

void* ApiFactory::GetClassByName(string apiName)
{
    auto it = m_ApiFactoryMap.find(apiName);
    if(it != m_ApiFactoryMap.end()) {
        return it->second();
    }
    return nullptr;
}

void ApiFactory::registerApi(string apiName,CreateApi createApi)
{
    m_ApiFactoryMap[apiName] = createApi;
}

unordered_map<string,CreateApi> ApiFactory::getApiFactoryMap()
{
    return m_ApiFactoryMap; 
}

