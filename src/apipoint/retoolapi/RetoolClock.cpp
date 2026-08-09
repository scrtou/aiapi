#include <apipoint/retoolapi/RetoolClock.h>

#include <thread>

namespace retool {
namespace {

class RealRetoolClock final : public IRetoolClock
{
  public:
    Clock::time_point now() const override { return Clock::now(); }

    void sleepFor(std::chrono::milliseconds duration) override
    {
        std::this_thread::sleep_for(duration);
    }
};

}  // namespace

std::shared_ptr<IRetoolClock> makeRealRetoolClock()
{
    return std::make_shared<RealRetoolClock>();
}

}  // namespace retool
