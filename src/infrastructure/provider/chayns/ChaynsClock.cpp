#include <infrastructure/provider/chayns/ChaynsClock.h>

#include <thread>

namespace chayns {
namespace {

class RealChaynsClock final : public IChaynsClock
{
  public:
    Clock::time_point now() const override { return Clock::now(); }

    void sleepFor(std::chrono::milliseconds duration) override
    {
        std::this_thread::sleep_for(duration);
    }
};

}  // namespace

std::shared_ptr<IChaynsClock> makeRealChaynsClock()
{
    return std::make_shared<RealChaynsClock>();
}

}  // namespace chayns
