#ifndef ACCOUNT_CLOCK_H
#define ACCOUNT_CLOCK_H

#include <chrono>
#include <memory>

namespace account {

class IAccountClock
{
  public:
    virtual ~IAccountClock() = default;
    virtual void sleepFor(std::chrono::milliseconds duration) = 0;
};

std::shared_ptr<IAccountClock> makeRealAccountClock();

}  // namespace account

#endif  // ACCOUNT_CLOCK_H
