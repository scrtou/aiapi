#ifndef IRETOOL_PROVISION_CLOCK_H
#define IRETOOL_PROVISION_CLOCK_H

#include <chrono>
#include <optional>
#include <string>

namespace retoolProvision {

/**
 * Wall-clock and persisted timestamp codec used by the Retool provision-health
 * policy.  The policy owns comparisons and cooldown duration; infrastructure
 * owns access to the system clock and the local DB timestamp representation.
 */
class IRetoolProvisionClock
{
  public:
    using TimePoint = std::chrono::system_clock::time_point;

    virtual ~IRetoolProvisionClock() = default;
    virtual TimePoint now() const = 0;
    virtual std::string formatLocalTimestamp(TimePoint value) const = 0;
    virtual std::optional<TimePoint> parseLocalTimestamp(
        const std::string& value) const = 0;
};

}  // namespace retoolProvision

#endif  // IRETOOL_PROVISION_CLOCK_H
