#include "ApiManager.h"
#include <ApiFactory.h>
ApiManager::ApiManager()
{
}

ApiManager::~ApiManager()   
{
}

void ApiManager::addApiInfo(std::shared_ptr<ApiInfo> apiInfo)
{
    m_ApiNameApiMap[apiInfo->apiName]=apiInfo;
    for(auto& modelname:apiInfo->api->ModelInfoMap)
    {
        m_ModelnameApiQueueMap[modelname.first].push(apiInfo);
    }
}

void ApiManager::updateApiInfo(std::shared_ptr<ApiInfo> apiInfo)
{

}

std::shared_ptr<ApiInfo> ApiManager::getApiInfoByApiName(string apiName)
{
    return m_ApiNameApiMap[apiName];
}

std::shared_ptr<ApiInfo> ApiManager::getApiInfoByModelName(string modelName)
{
    return m_ModelnameApiQueueMap[modelName].top();
}

void ApiManager::disableApiByApiName(const string& apiName)
{
    if(m_ApiNameApiMap[apiName]!=nullptr)
    {
        m_ApiNameApiMap[apiName]->status=false;
    }
}

void ApiManager::disableModel_Api(const string& modelName,const string& apiName)
{
    if(m_ApiNameApiMap[apiName]!=nullptr)
    {
        m_ApiNameApiMap[apiName]->api->ModelInfoMap[modelName].status=false;
    }
}
void ApiManager::enableApiByApiName(const string& apiName)
{
    if(m_ApiNameApiMap[apiName]!=nullptr)
    {
        m_ApiNameApiMap[apiName]->status=true;
    }
}
void ApiManager::enableModel_Api(const string& modelName,const string& apiName)
{
    if(m_ApiNameApiMap[apiName]!=nullptr)
    {
        m_ApiNameApiMap[apiName]->api->ModelInfoMap[modelName].status=true;
    }
}
void ApiManager::flushModelnameApiQueueMap(const string& modelName)
{
    auto tmp =m_ModelnameApiQueueMap[modelName].top();
    m_ModelnameApiQueueMap[modelName].pop();
    m_ModelnameApiQueueMap[modelName].push(tmp);
}
void ApiManager::init()
{
    LOG_INFO << "ApiManager::init start";
    auto customConfig = app().getCustomConfig();
    auto reverseApiList = customConfig["reverse_api_list"];
    //LOG_INFO << "reverseApiList size: " << reverseApiList.size();
    LOG_INFO << " ApiFactory::m_ApiFactoryMap size: " << ApiFactory::getInstance().getApiFactoryMap().size();
    for(auto& apiName : ApiFactory::getInstance().getApiFactoryMap())
    {
        if(apiName.second==nullptr)
        {
            LOG_INFO << "api: " << apiName.first << " is nullptr";
            continue;
        }
        void* api = ApiFactory::getInstance().GetClassByName(apiName.first);
        if(api!=nullptr)
        {
            addApiInfo(make_shared<ApiInfo>(apiName.first,std::shared_ptr<APIinterface>(static_cast<APIinterface*>(api))));
        }
        else
        {
            LOG_INFO << "api: " << apiName.first << " is nullptr";
        }
    }
    LOG_INFO << "ApiManager::init end";
    LOG_INFO << "m_ApiNameApiMap size: " << m_ApiNameApiMap.size();
}

ApiManager& ApiManager::getInstance()
{
    static ApiManager instance;
    return instance;
}
std::shared_ptr<APIinterface> ApiManager::getApiByApiName(string apiName)
{
    auto it = m_ApiNameApiMap.find(apiName);
    if(it==m_ApiNameApiMap.end())
    {
        LOG_ERROR << "apiName: " << apiName << " is not found";
        return nullptr;
    }
    return it->second->api;
}