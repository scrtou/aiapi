#pragma once

#include <domain/model/ErrorEvent.h>

namespace metrics {

class ITelemetrySink
{
  public:
    virtual ~ITelemetrySink() = default;
    virtual void record(const ErrorEvent& event) = 0;
    virtual void recordRequestCompleted(const RequestCompletedEvent& event) = 0;
};

}  // namespace metrics
