#ifndef RETOOL_CLOCK_H
#define RETOOL_CLOCK_H

#include <chrono>
#include <memory>

namespace retool {

/** Clock seam used only for Retool polling waits. */
class IRetoolClock
{
  public:
    using Clock = std::chrono::steady_clock;
    virtual ~IRetoolClock() = default;
    virtual Clock::time_point now() const = 0;
    virtual void sleepFor(std::chrono::milliseconds duration) = 0;
};

std::shared_ptr<IRetoolClock> makeRealRetoolClock();

}  // namespace retool

#endif  // RETOOL_CLOCK_H
