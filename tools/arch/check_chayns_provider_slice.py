#!/usr/bin/env python3
"""P6-W2 ratchet for the Chayns Provider vertical slice.

P6-W3 removed the temporary APIinterface/session_st lane.  This gate keeps the
completed registry contract while continuing to prevent Chayns from regressing
away from its ProviderBase/Result implementation.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
from typing import Mapping


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4

CHAYNS_HEADER = SRC / "infrastructure/provider/chayns/chaynsapi.h"
CHAYNS_CPP = SRC / "infrastructure/provider/chayns/chaynsapi.cpp"
RETOOL_HEADER = SRC / "infrastructure/provider/retool/retoolapi.h"
WIRING = SRC / "runtime/AppWiring.cpp"
GENERATION_PIPELINE = SRC / "sessionManager/core/GenerationPipeline.cpp"
SESSION = SRC / "sessionManager/core/Session.cpp"
REAPER = SRC / "infrastructure/provider/chayns/chaynsThreadReaper.cpp"
REGISTRY_PORT = SRC / "domain/port/IProviderRegistry.h"
EXECUTION_GATE = SRC / "domain/port/IExecutionGate.h"
SESSION_GATE = SRC / "sessionManager/core/SessionExecutionGate.h"
FIXTURE_TEST = SRC / "test/test_chayns_provider_fixture.cpp"
REGISTRY_TEST = SRC / "test/test_provider_registry_port.cpp"


def uncommented(text: str) -> str:
    """Remove comments so migration notes cannot accidentally trip a rule."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def validate(overrides: Mapping[Path, str] | None = None) -> list[str]:
    overrides = overrides or {}
    errors: list[str] = []

    def read(path: Path) -> str:
        if path in overrides:
            return overrides[path]
        if not path.is_file():
            errors.append(f"missing required P6-W2 file: {path.relative_to(ROOT)}")
            return ""
        return path.read_text(errors="replace")

    header = read(CHAYNS_HEADER)
    chayns_cpp = read(CHAYNS_CPP)
    wiring = read(WIRING)
    generation_pipeline = read(GENERATION_PIPELINE)
    session = read(SESSION)
    reaper = read(REAPER)
    registry_port = read(REGISTRY_PORT)
    execution_gate = read(EXECUTION_GATE)
    session_gate = read(SESSION_GATE)
    fixture_test = read(FIXTURE_TEST)
    registry_test = read(REGISTRY_TEST)

    # The provider itself, not merely its happy-path adapter, must be detached
    # from the old aggregate and project service locators.
    provider_sources: list[Path] = []
    chayns_dir = SRC / "infrastructure/provider/chayns"
    if chayns_dir.is_dir():
        for path in sorted(chayns_dir.rglob("*")):
            if path.suffix in {".h", ".hpp", ".cpp", ".cc"}:
                provider_sources.append(path)
    else:
        errors.append("missing src/infrastructure/provider/chayns directory")

    forbidden_literals = (
        "session_st",
        "APIinterface",
        "Session.h",
        "session.response",
    )
    singleton = re.compile(r"::\s*(?:getInstance|instance)\s*\(")
    for path in provider_sources:
        code = uncommented(read(path))
        relative = path.relative_to(ROOT)
        for forbidden in forbidden_literals:
            if forbidden in code:
                errors.append(f"{relative} revived legacy Chayns dependency: {forbidden}")
        if singleton.search(code):
            errors.append(f"{relative} revived a project singleton lookup")

    # P8 closes the last source-directory transition: active providers are
    # infrastructure adapters, not an ``apipoint`` exception that happens to
    # include ProviderBase.  Do not reintroduce compatibility forwarding
    # headers at the old location; they would hide a second physical owner
    # from the source-ownership and include-path gates.
    legacy_provider_root = SRC / "apipoint"
    if legacy_provider_root.exists():
        errors.append("src/apipoint must stay absent; providers belong under src/infrastructure/provider")
    for path in (CHAYNS_HEADER, RETOOL_HEADER):
        if "infrastructure/provider/" not in path.relative_to(SRC).as_posix():
            errors.append(f"provider header is outside final infrastructure location: {path.relative_to(ROOT)}")

    if not re.search(
        r"class\s+chaynsapi\s+final\s*:\s*public\s+provider::ProviderBase",
        header,
    ):
        errors.append("chaynsapi must directly derive from provider::ProviderBase")
    for needle in (
        "public provider::IProviderModelCatalog",
        "public provider::IProviderThreadContext",
        "platform::Result<void> initialize()",
        "doGenerate(",
        "sendWithinContext(",
    ):
        if needle not in header:
            errors.append(f"Chayns P6-W2 provider contract missing: {needle}")
    for needle in (
        "provider::ProviderRequest",
        "provider::ProviderCallContext",
        "platform::Result<provider::ProviderResponse>",
        "interruptionError(context)",
        "sendWithinContext(",
    ):
        if needle not in chayns_cpp:
            errors.append(f"Chayns P6-W2 implementation missing: {needle}")

    # Composition must register Chayns only through the complete narrow lane.
    for needle in (
        "provider::makeProductionProvider<chaynsapi>",
        "chayns->initialize()",
        'registerChatProvider("chaynsapi", chayns, chayns, chayns)',
        "registry->freeze()",
    ):
        if needle not in wiring:
            errors.append(f"runtime Chayns slice wiring missing: {needle}")
    clean_wiring = uncommented(wiring)
    if re.search(r"\bregisterProvider\s*\(", clean_wiring):
        errors.append("runtime must not revive the deleted legacy provider lane")

    # The application only resolves the narrow lane.  P7-W1 moved this seam
    # into GenerationPipeline; remaining legacy session JSON is materialized
    # after a Result, never handed to a provider.
    invoke_provider_at = generation_pipeline.find("GenerationPipeline::invokeProvider")
    invoke_provider = (generation_pipeline[invoke_provider_at:]
                       if invoke_provider_at >= 0 else "")
    if "findChatProvider(session.request.api)" not in invoke_provider:
        errors.append("GenerationPipeline no longer resolves IChatProvider")
    if "findProvider(" in uncommented(invoke_provider):
        errors.append("GenerationPipeline revived a legacy provider fallback")
    for needle in (
        "ProviderCallContext context{cancellation, deadline}",
        "return result.error()",
        "applyProviderResponse(session, result.value())",
    ):
        if needle not in generation_pipeline:
            errors.append(f"GenerationPipeline Chayns Result bridge missing: {needle}")

    # Model and upstream-thread ownership are separate capabilities. Session
    # cleanup/rebind and the reaper must never rediscover a provider through a
    # wide port.
    if "findModelCatalog(provider)" not in read(SRC / "sessionManager/core/AiApiUseCase.cpp"):
        errors.append("AiApiUseCase does not resolve the narrow model catalog")
    if "findThreadContext(providerName)" not in session:
        errors.append("Session thread cleanup/transfer must use IProviderThreadContext")
    if "findProvider(" in uncommented(session):
        errors.append("Session revived a legacy provider lookup")
    if "findThreadContext(\"chaynsapi\")" not in reaper:
        errors.append("chaynsThreadReaper must use IProviderThreadContext")
    if "findProvider(" in uncommented(reaper):
        errors.append("chaynsThreadReaper revived a legacy provider lookup")

    for needle in (
        "findChatProvider",
        "findModelCatalog",
        "findThreadContext",
    ):
        if needle not in registry_port:
            errors.append(f"IProviderRegistry narrow capability missing: {needle}")
    if "findProvider(" in uncommented(registry_port) or "APIinterface" in uncommented(registry_port):
        errors.append("IProviderRegistry revived a legacy provider capability")

    # CancelPrevious hands cancellation authority to the gate, never a
    # provider. A stale cancelled request must not release its replacement.
    for needle in (
        "CancellationSourcePtr& outCancellation",
        "release(const std::string& sessionKey,",
        "const CancellationSourcePtr& cancellation",
    ):
        if needle not in execution_gate:
            errors.append(f"IExecutionGate request-cancellation contract missing: {needle}")
    for needle in (
        "currentCancellation->request()",
        "slot->currentCancellation != cancellation",
        "platform::CancellationToken cancellationToken() const",
    ):
        if needle not in session_gate:
            errors.append(f"SessionExecutionGate cancellation ownership missing: {needle}")

    # These focused tests prevent the ratchet from becoming a structure-only
    # claim: fixture cancellation and error mapping exercise the real Chayns
    # port, while session tests exercise the thread capability.
    for needle in (
        "ChaynsProvider_CancellationStopsBeforeTheNextPollingBoundary",
        "ChaynsProvider_GenerationServicePreservesProviderErrorCodeForTransport",
        "registerChatProvider(\"chaynsapi\"",
    ):
        if needle not in fixture_test:
            errors.append(f"Chayns P6-W2 fixture coverage missing: {needle}")
    for needle in (
        "ProviderRegistryPort_ResolvesNarrowCapabilitiesWithoutLegacyFallback",
        "ProviderRegistryPort_SessionUsesNarrowThreadContextForTransferAndCleanup",
    ):
        if needle not in registry_test:
            errors.append(f"P6-W2 registry/thread-context coverage missing: {needle}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="P6-W2 Chayns provider slice ratchet")
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="also prove the gate detects an in-memory ProviderBase mutation",
    )
    args = parser.parse_args()

    errors = validate()
    if errors:
        print("P6-W2 Chayns Provider slice gate FAIL")
        for error in errors:
            print(f"  {error}")
        return FAIL

    if args.selftest:
        original = CHAYNS_HEADER.read_text(errors="replace")
        mutation = original.replace("public provider::ProviderBase", "public BrokenProviderBase", 1)
        if mutation == original:
            print("P6-W2 Chayns Provider slice gate selftest FAIL")
            print("  probe could not mutate chaynsapi ProviderBase inheritance")
            return FAIL
        mutation_errors = validate({CHAYNS_HEADER: mutation})
        if not mutation_errors:
            print("P6-W2 Chayns Provider slice gate selftest FAIL")
            print("  inheritance mutation unexpectedly passed")
            return FAIL
        print("PASS P6-W2 Chayns Provider slice gate selftest: inheritance mutation was rejected")

    print("PASS P6-W2: Chayns stays on the complete narrow ProviderBase/Result registry contract")
    return 0


if __name__ == "__main__":
    sys.exit(main())
