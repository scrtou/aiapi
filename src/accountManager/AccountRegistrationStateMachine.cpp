#include <accountManager/AccountRegistrationStateMachine.h>

namespace account {

AccountRegistrationStateMachine::AccountRegistrationStateMachine(IAccountStore* store)
    : store_(store)
{
}

void AccountRegistrationStateMachine::setStore(IAccountStore* store)
{
    std::lock_guard<std::mutex> lock(mutex_);
    store_ = store;
}

int AccountRegistrationStateMachine::begin(const std::string& apiName)
{
    IAccountStore* store = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        store = store_;
    }
    if (!store) {
        return -1;
    }

    const int waitingId = store->createWaitingAccount(apiName);
    if (waitingId < 0) {
        return -1;
    }
    if (!store->updateAccountStatusById(waitingId, AccountStatus::REGISTERING)) {
        // No incomplete reservation may leak if the transition itself failed.
        store->updateAccountStatusById(waitingId, AccountStatus::WAITING);
        store->deleteWaitingAccount(waitingId);
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registeringIds_.insert(waitingId);
    }
    return waitingId;
}

void AccountRegistrationStateMachine::rollback(int waitingId)
{
    IAccountStore* store = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        store = store_;
    }
    if (!store || waitingId < 0) {
        return;
    }
    // Preserve this order.  deleteWaitingAccount intentionally only accepts a
    // waiting row, so reversing these calls silently leaks the reservation.
    store->updateAccountStatusById(waitingId, AccountStatus::WAITING);
    store->deleteWaitingAccount(waitingId);
}

bool AccountRegistrationStateMachine::activate(int waitingId, const Accountinfo_st& account)
{
    IAccountStore* store = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        store = store_;
    }
    return store && waitingId >= 0 && store->activateAccount(waitingId, account);
}

void AccountRegistrationStateMachine::finish(int waitingId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    registeringIds_.erase(waitingId);
}

bool AccountRegistrationStateMachine::isRegistering(int waitingId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return registeringIds_.count(waitingId) != 0;
}

bool AccountRegistrationStateMachine::isRegisteringByUsername(const std::string& apiName,
                                                               const std::string& userName) const
{
    IAccountStore* store = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        store = store_;
    }
    return store && store->getAccountStatusByUsername(apiName, userName) ==
                        AccountStatus::REGISTERING;
}

}  // namespace account
