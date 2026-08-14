#pragma once

#include <domain/model/AccountData.h>

class IAccountSettingsQuery
{
  public:
    virtual ~IAccountSettingsQuery() = default;
    virtual AccountAutomationSettings getAccountAutomationSettings() const = 0;
};
