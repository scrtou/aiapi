#include "ApiFactory.h"
#include "./apipoint/APIinterface.h"
#include <memory>
#include <unordered_map>
#include <queue>
#include <string>
#include <vector>

using std::priority_queue;
using std::shared_ptr;
using std::string;
using std::unordered_map;
using std::vector;
using std::make_shared;

struct ApiInfo
{
    string apiName;
   shared_ptr<APIinterface> api;
   bool status=true;
   int apiUseCount=0;
   ApiInfo(string apiName,shared_ptr<APIinterface> api):apiName(apiName),api(api){}
};
class Compare
{
public:
    bool operator()(const std::shared_ptr<ApiInfo>& a, const std::shared_ptr<ApiInfo>& b)
    {
        return a->apiUseCount > b->apiUseCount;
    }
};
class ApiManager
{
private:
    /* 成员数据 */
    unordered_map<string, std::priority_queue<std::shared_ptr<ApiInfo>,vector<std::shared_ptr<ApiInfo>>,Compare>> m_ModelnameApiQueueMap;
    unordered_map<string,std::shared_ptr<ApiInfo>> m_ApiNameApiMap;
public:
    ApiManager();
    ~ApiManager();
    void init();
    static ApiManager& getInstance();
    void addApiInfo(std::shared_ptr<ApiInfo> apiInfo);
    void updateApiInfo(std::shared_ptr<ApiInfo> apiInfo);
    std::shared_ptr<ApiInfo> getApiInfoByApiName(string apiName);
    std::shared_ptr<ApiInfo> getApiInfoByModelName(string modelName);
    std::shared_ptr<APIinterface> getApiByApiName(string apiName);
    void  disableApiByApiName(const string& apiName);
    void disableModel_Api(const string& modelName,const string& apiName);
    void enableApiByApiName(const string& apiName);
    void enableModel_Api(const string& modelName,const string& apiName);
   void flushModelnameApiQueueMap(const string& modelName);
};
