#include <infrastructure/provider/retool/RetoolClock.h>

#include <algorithm>
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

    bool sleepFor(std::chrono::milliseconds duration,
                  const std::function<bool()>& interrupted) override
    {
        constexpr auto kSlice = std::chrono::milliseconds(20);
        auto remaining = duration;
        while (remaining > std::chrono::milliseconds::zero()) {
            if (interrupted && interrupted()) return false;
            const auto slice = std::min(kSlice, remaining);
            std::this_thread::sleep_for(slice);
            remaining -= slice;
        }
        return !interrupted || !interrupted();
    }
};

}  // namespace

std::shared_ptr<IRetoolClock> makeRealRetoolClock()
{
    return std::make_shared<RealRetoolClock>();
}

}  // namespace retool
