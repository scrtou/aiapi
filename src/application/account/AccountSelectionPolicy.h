#pragma once

#include <domain/model/AccountData.h>

#include <memory>
#include <set>
#include <string>

namespace account::selection {

bool isPoolMember(const std::shared_ptr<Accountinfo_st>& candidate);
bool matchesRequirement(const std::shared_ptr<Accountinfo_st>& candidate,
                        const std::string& apiName,
                        AccountRequirement requirement,
                        const std::set<std::string>& excludedUsers);
const char* requirementName(AccountRequirement requirement);

}  // namespace account::selection
