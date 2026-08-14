#pragma once

#include <domain/model/AccountData.h>

#include <map>
#include <memory>
#include <string>

class IAccountCatalog
{
  public:
    using AccountMap = std::map<std::string,
        std::map<std::string, std::shared_ptr<Accountinfo_st>>>;
    virtual ~IAccountCatalog() = default;
    virtual AccountMap listAccounts() = 0;
    virtual void checkChannelAccountCount(std::string apiName) = 0;
};
