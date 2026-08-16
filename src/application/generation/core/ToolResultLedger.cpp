#include <application/generation/core/ToolResultLedger.h>

#include <algorithm>
#include <utility>

namespace generation::toolresult {
namespace {

constexpr std::size_t kMaxLedgerEntries = 256;
constexpr std::size_t kMaxReplayableInputBytes = 512 * 1024;

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

bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() &&
        value.compare(0, prefix.size(), prefix) == 0;
}

struct ReplayableTextSnapshot {
    std::string text;
    bool hasText = false;
    bool auxiliary = false;
    bool overflow = false;
};

ReplayableTextSnapshot collectReplayableText(const std::vector<CurrentInputPart>& parts)
{
    ReplayableTextSnapshot snapshot;
    for (const auto& part : parts) {
        if (part.isToolResult || !part.isReplayableText) continue;
        snapshot.hasText = true;
        snapshot.auxiliary = snapshot.auxiliary || part.isAuxiliary;
        if (part.text.size() > kMaxReplayableInputBytes - snapshot.text.size()) {
            snapshot.overflow = true;
            snapshot.text.clear();
            return snapshot;
        }
        snapshot.text += part.text;
    }
    return snapshot;
}

void replaceReplayableTextSnapshot(session_st& session,
                                   const ReplayableTextSnapshot& snapshot)
{
    if (!snapshot.hasText || snapshot.auxiliary) return;

    // A snapshot is used only for exact prefix removal.  Clearing an
    // unexpectedly huge value is safer than retaining a partial prefix and
    // cutting a later user message at the wrong offset.
    if (snapshot.overflow || snapshot.text.size() > kMaxReplayableInputBytes) {
        session.provider.replayableInputTextSnapshot.clear();
        return;
    }
    session.provider.replayableInputTextSnapshot = snapshot.text;
}

}  // namespace

ReconciliationResult reconcileCurrentInput(
    session_st& session,
    const std::vector<CurrentInputPart>& parts)
{
    ReconciliationResult result;
    if (parts.empty()) return result;

    const ReplayableTextSnapshot replayableText = collectReplayableText(parts);
    std::size_t replayableTextBytesToSuppress = 0;
    if (!replayableText.text.empty() &&
        !session.provider.replayableInputTextSnapshot.empty() &&
        startsWith(replayableText.text, session.provider.replayableInputTextSnapshot)) {
        replayableTextBytesToSuppress =
            session.provider.replayableInputTextSnapshot.size();
        result.suppressedReplayTextBytes = replayableTextBytesToSuppress;
    }

    std::string rebuilt;
    for (const auto& part : parts) {
        if (!part.isToolResult && part.isReplayableText) {
            const std::size_t skipped = std::min(
                replayableTextBytesToSuppress, part.text.size());
            replayableTextBytesToSuppress -= skipped;
            rebuilt.append(part.text, skipped, std::string::npos);
            continue;
        }

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

    replaceReplayableTextSnapshot(session, replayableText);

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
