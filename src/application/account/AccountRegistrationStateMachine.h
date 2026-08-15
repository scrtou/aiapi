#pragma once

#include <domain/port/IAccountStore.h>

#include <mutex>
#include <set>
#include <string>

namespace account {

/**
 * Owns only durable registration transitions and the in-flight ID set.
 *
 * `waiting -> registering -> active` is driven by the workflow; every failed
 * edge goes through rollback(), which deliberately restores `waiting` before
 * deleting because the repository's delete query is state guarded.
 */
class AccountRegistrationStateMachine
{
  public:
    explicit AccountRegistrationStateMachine(IAccountStore* store = nullptr);

    void setStore(IAccountStore* store);
    int begin(const std::string& apiName);
    void rollback(int waitingId);
    bool activate(int waitingId, const Accountinfo_st& account);
    void finish(int waitingId);

    bool isRegistering(int waitingId) const;
    bool isRegisteringByUsername(const std::string& apiName,
                                 const std::string& userName) const;

  private:
    IAccountStore* store_ = nullptr;
    mutable std::mutex mutex_;
    std::set<int> registeringIds_;
};

}  // namespace account
