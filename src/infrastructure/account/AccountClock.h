#ifndef ACCOUNT_CLOCK_H
#define ACCOUNT_CLOCK_H

#include <domain/port/IAccountClock.h>

#include <memory>

namespace account {

/** Construct the concrete system-clock adapter at the runtime boundary. */
std::shared_ptr<IAccountClock> makeRealAccountClock();

}  // namespace account

#endif  // ACCOUNT_CLOCK_H
