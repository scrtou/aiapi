#pragma once

#include <domain/model/ChannelInfo.h>
#include <domain/port/IBackgroundExecutor.h>

#include <list>
#include <string>

/**
 * Controller-facing channel administration workflow.
 *
 * The transport layer needs one coherent operation for each admin endpoint:
 * validation/policy decisions, built-in channel preservation, persistence and
 * the best-effort account recount must not be split between a Controller and
 * three unrelated runtime services.
 */
enum class ChannelAdminOutcome {
    Success,
    Failed,
    ProviderRetired,
    ServiceUnavailable,
    BuiltInChannelNotFound,
};

struct ChannelAdminResult {
    ChannelAdminOutcome outcome = ChannelAdminOutcome::Failed;
    Channelinfo_st channel;
    // Recount happens only after a successful write.  Its rejection does not
    // roll back an already committed channel operation, but remains observable.
    TaskSubmitResult recountSubmission = TaskSubmitResult::Accepted;

    bool succeeded() const { return outcome == ChannelAdminOutcome::Success; }
};

class IChannelAdminUseCase
{
  public:
    virtual ~IChannelAdminUseCase() = default;

    virtual std::list<Channelinfo_st> listChannels() const = 0;
    virtual ChannelAdminResult add(Channelinfo_st channel) = 0;
    virtual ChannelAdminResult update(Channelinfo_st channel) = 0;
    virtual ChannelAdminResult remove(int channelId) = 0;
    virtual ChannelAdminResult updateStatus(std::string channelName, bool status) = 0;
};
