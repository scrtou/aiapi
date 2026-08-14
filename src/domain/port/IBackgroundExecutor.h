#pragma once

#include <functional>
#include <string>

enum class TaskSubmitResult { Accepted, QueueFull, ShuttingDown, Stopped };

inline const char* toString(TaskSubmitResult result)
{
    switch (result) {
        case TaskSubmitResult::Accepted: return "Accepted";
        case TaskSubmitResult::QueueFull: return "QueueFull";
        case TaskSubmitResult::ShuttingDown: return "ShuttingDown";
        case TaskSubmitResult::Stopped: return "Stopped";
    }
    return "Unknown";
}

class IBackgroundExecutor
{
  public:
    virtual ~IBackgroundExecutor() = default;
    [[nodiscard]] virtual TaskSubmitResult submit(
        const std::string& name, std::function<void()> task) = 0;
};
