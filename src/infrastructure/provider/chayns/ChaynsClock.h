#ifndef CHAYNS_CLOCK_H
#define CHAYNS_CLOCK_H

#include <chrono>
#include <memory>

namespace chayns {

class IChaynsClock
{
  public:
    using Clock = std::chrono::steady_clock;
    virtual ~IChaynsClock() = default;
    virtual Clock::time_point now() const = 0;
    virtual void sleepFor(std::chrono::milliseconds duration) = 0;
};

std::shared_ptr<IChaynsClock> makeRealChaynsClock();

}  // namespace chayns

#endif  // CHAYNS_CLOCK_H
