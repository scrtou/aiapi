#ifndef RETOOL_CLOCK_H
#define RETOOL_CLOCK_H

#include <chrono>
#include <functional>
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

    /**
     * Deadline/cancellation-aware wait seam.  Fixture clocks inherit the
     * compatibility implementation (one deterministic virtual sleep), while
     * the production clock overrides it with short interruptible slices.
     */
    virtual bool sleepFor(std::chrono::milliseconds duration,
                          const std::function<bool()>& interrupted)
    {
        if (interrupted && interrupted()) return false;
        sleepFor(duration);
        return !interrupted || !interrupted();
    }
};

std::shared_ptr<IRetoolClock> makeRealRetoolClock();

}  // namespace retool

#endif  // RETOOL_CLOCK_H
