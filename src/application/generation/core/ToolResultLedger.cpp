#include <application/generation/core/ToolResultLedger.h>

#include <algorithm>
#include <utility>

namespace generation::toolresult {
namespace {

constexpr std::size_t kMaxLedgerEntries = 256;

bool contains(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

void eraseValue(std::vector<std::string>& values, const std::string& value)
{
    values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

void appendBoundedUnique(std::vector<std::string>& values, const std::string& value)
{
    if (value.empty() || contains(values, value)) return;
    values.push_back(value);
    if (values.size() > kMaxLedgerEntries) values.erase(values.begin());
}

}  // namespace

ReconciliationResult reconcileCurrentInput(
    session_st& session,
    const std::vector<CurrentInputPart>& parts)
{
    ReconciliationResult result;
    if (parts.empty()) return result;

    std::string rebuilt;
    for (const auto& part : parts) {
        if (!part.isToolResult || part.toolResultCallId.empty()) {
            rebuilt += part.text;
            continue;
        }

        const std::string& id = part.toolResultCallId;
        if (contains(session.provider.consumedToolResultIds, id)) {
            ++result.suppressedDuplicate;
            continue;
        }

        const bool wasPending = contains(session.provider.pendingToolCallIds, id);
        rebuilt += part.text;
        ++result.accepted;
        if (!wasPending) ++result.acceptedWithoutPendingCall;
        eraseValue(session.provider.pendingToolCallIds, id);
        appendBoundedUnique(session.provider.consumedToolResultIds, id);
    }

    // materializeRequest() runs before session continuity is resolved.  The
    // reconciled value must replace both fields before the tool bridge and the
    // provider invocation consume them.
    session.request.message = rebuilt;
    session.request.rawMessage = std::move(rebuilt);
    return result;
}

void recordEmittedToolCallIds(session_st& session,
                              const std::vector<std::string>& ids)
{
    for (const auto& id : ids) {
        if (id.empty()) continue;

        // IDs emitted after a restart or native provider retry may be reused
        // by an upstream implementation.  Treat a fresh emission as a new
        // pending result rather than letting an old consumed entry suppress it.
        eraseValue(session.provider.consumedToolResultIds, id);
        appendBoundedUnique(session.provider.pendingToolCallIds, id);
    }
}

}  // namespace generation::toolresult
