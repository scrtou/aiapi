#pragma once

#include <domain/model/AccountData.h>

#include <list>

class IAccountBackupStore
{
  public:
    virtual ~IAccountBackupStore() = default;
    virtual std::list<Accountinfo_st> listBackupAccounts() = 0;
};
