#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <list>
#include "Model.h"
#include<drogon/drogon.h>
using namespace drogon;
class ModelsManager {
public:
    ModelsManager();
    ~ModelsManager();
    void loadModels();
    Json::Value getModelsOpenai();
;
private:
    std::map<std::string, std::list<std::shared_ptr<Model>>> models;//api--models
};

