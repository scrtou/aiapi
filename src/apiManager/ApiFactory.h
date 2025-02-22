#ifndef APIFACTORY_H
#define APIFACTORY_H
#include <string>
#include "Apicomn.h"
#include <unordered_map>
#include <memory>
#include "../apipoint/APIinterface.h"
using namespace std;
typedef void* (*CreateApi)(void);
class ApiFactory
{
private:
    /* data */
    unordered_map<string,CreateApi> m_ApiFactoryMap;
public:
   static ApiFactory& getInstance();
   void* GetClassByName(string apiName);
   void registerApi(string apiName,CreateApi mothod);
   unordered_map<string,CreateApi> getApiFactoryMap();
};

class ApiRegister
{
public:
    ApiRegister(string apiName,CreateApi mothod)
    {
        ApiFactory::getInstance().registerApi(apiName,mothod);
    }
};

#define DEClARE_RUNTIME(class_name) \
    static std::shared_ptr<ApiRegister> class_name##pg
#define IMPLEMENT_RUNTIME(class_name,classcommon) \
    std::shared_ptr<ApiRegister> class_name##pg = \
        std::make_shared<ApiRegister>(#class_name,classcommon::createApi);
#endif