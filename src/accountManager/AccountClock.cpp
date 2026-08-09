#include <accountManager/AccountClock.h>

#include <thread>

namespace account {
namespace {

class RealAccountClock final : public IAccountClock
{
  public:
    void sleepFor(std::chrono::milliseconds duration) override
    {
        std::this_thread::sleep_for(duration);
    }
};

}  // namespace

std::shared_ptr<IAccountClock> makeRealAccountClock()
{
    return std::make_shared<RealAccountClock>();
}

}  // namespace account
