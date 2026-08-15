#ifndef RETOOL_PROVISION_CLOCK_H
#define RETOOL_PROVISION_CLOCK_H

#include <domain/port/IRetoolProvisionClock.h>

#include <memory>

namespace retoolProvision {

/** Create the process wall-clock adapter used by runtime composition. */
std::shared_ptr<IRetoolProvisionClock> makeSystemRetoolProvisionClock();

}  // namespace retoolProvision

#endif  // RETOOL_PROVISION_CLOCK_H
