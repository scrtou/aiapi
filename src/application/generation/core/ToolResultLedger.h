#pragma once

#include <application/generation/contracts/GenerationRequest.h>
#include <application/generation/contracts/GenerationSession.h>

#include <cstddef>
#include <string>
#include <vector>

namespace generation::toolresult {

struct ReconciliationResult {
    std::size_t accepted = 0;
    std::size_t suppressedDuplicate = 0;
    std::size_t acceptedWithoutPendingCall = 0;
    std::size_t suppressedReplayTextBytes = 0;
};

/**
 * Rebuild the provider-facing current input after session resolution.
 *
 * A protocol adapter supplies opaque text fragments plus canonical tool-result
 * identities.  This service never parses protocol JSON or bridge text: it
 * only suppresses result fragments whose IDs have already been accepted and
 * text prefixes that an adapter explicitly marks as an append-only replay.
 */
ReconciliationResult reconcileCurrentInput(
    session_st& session,
    const std::vector<CurrentInputPart>& parts);

/** Record final tool-call IDs emitted to a client and awaiting a result. */
void recordEmittedToolCallIds(session_st& session,
                              const std::vector<std::string>& ids);

}  // namespace generation::toolresult
