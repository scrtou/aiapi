#pragma once

#include <infrastructure/provider/chayns/ChaynsProvider.h>

// Backward-compatible source name for composition/tests while callers migrate
// to the explicit provider-orchestrator type.
using chaynsapi = ChaynsProvider;
