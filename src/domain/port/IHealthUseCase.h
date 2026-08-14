#pragma once

#include <cstddef>
#include <cstdint>

struct HealthReadiness
{
    bool database = false;
    bool provider = false;
    bool account = false;
    std::size_t accountCount = 0;

    bool ready() const { return database && provider && account; }
};

/** Controller-facing health/readiness workflow. */
class IHealthUseCase
{
  public:
    virtual ~IHealthUseCase() = default;

    virtual std::uint64_t uptimeSeconds() const = 0;
    virtual HealthReadiness readiness() const = 0;
};
