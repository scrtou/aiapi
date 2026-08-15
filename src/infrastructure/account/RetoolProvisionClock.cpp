#include <infrastructure/account/RetoolProvisionClock.h>

#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace {

constexpr const char* kDbTimestampFormat = "%Y-%m-%d %H:%M:%S";

// std::localtime/std::mktime share process timezone state.  Serialising the
// codec keeps the portable fallback safe without leaking a platform API into
// the domain port.
std::mutex& localTimeMutex()
{
    static std::mutex mutex;
    return mutex;
}

class SystemRetoolProvisionClock final : public retoolProvision::IRetoolProvisionClock
{
  public:
    TimePoint now() const override
    {
        return std::chrono::system_clock::now();
    }

    std::string formatLocalTimestamp(TimePoint value) const override
    {
        const std::time_t raw = std::chrono::system_clock::to_time_t(value);
        std::lock_guard<std::mutex> lock(localTimeMutex());
        const std::tm* local = std::localtime(&raw);
        if (local == nullptr) return {};

        std::ostringstream output;
        output << std::put_time(local, kDbTimestampFormat);
        return output.str();
    }

    std::optional<TimePoint> parseLocalTimestamp(const std::string& value) const override
    {
        std::tm local{};
        local.tm_isdst = -1;
        std::istringstream input(value);
        input >> std::get_time(&local, kDbTimestampFormat);
        if (input.fail()) return std::nullopt;
        input >> std::ws;
        if (!input.eof()) return std::nullopt;

        std::lock_guard<std::mutex> lock(localTimeMutex());
        const std::time_t raw = std::mktime(&local);
        if (raw == static_cast<std::time_t>(-1)) return std::nullopt;

        const auto parsed = std::chrono::system_clock::from_time_t(raw);
        // mktime normalises invalid dates.  Reject a value that does not round
        // trip byte-for-byte instead of silently accepting a different date.
        const std::tm* normalised = std::localtime(&raw);
        if (normalised == nullptr) return std::nullopt;
        std::ostringstream roundTrip;
        roundTrip << std::put_time(normalised, kDbTimestampFormat);
        if (roundTrip.str() != value) return std::nullopt;
        return parsed;
    }
};

}  // namespace

namespace retoolProvision {

std::shared_ptr<IRetoolProvisionClock> makeSystemRetoolProvisionClock()
{
    return std::make_shared<SystemRetoolProvisionClock>();
}

}  // namespace retoolProvision
