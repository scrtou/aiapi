#pragma once

#include <chrono>

namespace account {

/** Application-facing clock port for account workflow retry waits. */
class IAccountClock
{
  public:
    virtual ~IAccountClock() = default;
    virtual void sleepFor(std::chrono::milliseconds duration) = 0;
};

}  // namespace account
