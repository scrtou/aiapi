#pragma once

#include <domain/model/AccountData.h>

#include <cstdint>
#include <memory>
#include <string>

namespace chayns::policy {

inline constexpr const char* kFreeOrigin = "https://sidekick.ki";
inline constexpr const char* kFreeReferer = "https://sidekick.ki/";
inline constexpr const char* kProOrigin = "https://mein.sidekick.ki";
inline constexpr const char* kProReferer = "https://mein.sidekick.ki/";

struct RequestRoute
{
    bool isPro = false;
    int threadTypeId = 8;
    std::int64_t workspaceUacId = 0;
    std::string origin = kFreeOrigin;
    std::string referer = kFreeReferer;
};

RequestRoute requestRouteForAccount(const Accountinfo_st& account);

bool isUsableAccount(const std::shared_ptr<Accountinfo_st>& account,
                     bool requiresPro);

bool postFailureMayHaveBeenAccepted(int statusCode);

}  // namespace chayns::policy
