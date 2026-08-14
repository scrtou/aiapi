#!/usr/bin/env python3
"""P6-W2 ratchet for the Chayns Provider vertical slice.

The old APIinterface/session_st lane remains temporarily for Retool only.
This gate prevents a Chayns regression from silently reconnecting it while
the application still carries that compatibility lane.
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

CHAYNS_HEADER = SRC / "apipoint/chaynsapi/chaynsapi.h"
CHAYNS_CPP = SRC / "apipoint/chaynsapi/chaynsapi.cpp"
WIRING = SRC / "runtime/AppWiring.cpp"
GENERATION = SRC / "sessionManager/core/GenerationService.cpp"
SESSION = SRC / "sessionManager/core/Session.cpp"
REAPER = SRC / "apipoint/chaynsapi/chaynsThreadReaper.cpp"
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
    generation = read(GENERATION)
    session = read(SESSION)
    reaper = read(REAPER)
    registry_port = read(REGISTRY_PORT)
    execution_gate = read(EXECUTION_GATE)
    session_gate = read(SESSION_GATE)
    fixture_test = read(FIXTURE_TEST)
    registry_test = read(REGISTRY_TEST)

    # The provider itself, not merely its happy-path adapter, must be detached
    # from the legacy aggregate and project service locators.
    provider_sources = []
    chayns_dir = SRC / "apipoint/chaynsapi"
    if chayns_dir.is_dir():
        for path in sorted(chayns_dir.rglob("*")):
            if path.suffix in {".h", ".hpp", ".cpp", ".cc"}:
                provider_sources.append(path)
    else:
        errors.append("missing src/apipoint/chaynsapi directory")

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

    # layer-rules permits apipoint -> infrastructure only because the active
    # Chayns implementation has not yet moved directories. Do not let that
    # module-level transition permission become a blanket dependency escape.
    infrastructure_includes: list[Path] = []
    for path in SRC.joinpath("apipoint").rglob("*"):
        if path.suffix not in {".h", ".hpp", ".cpp", ".cc"}:
            continue
        if re.search(r"#\s*include\s*[<\"]infrastructure/", read(path)):
            infrastructure_includes.append(path)
    if infrastructure_includes != [CHAYNS_HEADER]:
        actual = ", ".join(str(path.relative_to(ROOT)) for path in infrastructure_includes) or "none"
        errors.append(
            "apipoint -> infrastructure transition edge must be only "
            f"chaynsapi.h -> ProviderBase.h; found: {actual}")

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

    # Composition must register Chayns only in the narrow lane. Retool is
    # deliberately still legacy until P6-W3, so preserve that explicit fact.
    for needle in (
        "provider::makeProductionProvider<chaynsapi>",
        "chayns->initialize()",
        'registerChatProvider("chaynsapi", chayns, chayns, chayns)',
        'registerProvider("retoolapi"',
        "registry->freeze()",
    ):
        if needle not in wiring:
            errors.append(f"runtime Chayns slice wiring missing: {needle}")
    if 'registerProvider("chaynsapi"' in uncommented(wiring):
        errors.append("Chayns must not be re-registered in the legacy APIinterface lane")

    # The application selects the narrow lane first and materializes legacy
    # JSON only after receiving its Result. This keeps Retool's temporary
    # fallback from becoming a route for Chayns.
    execute_provider_at = generation.find("GenerationService::executeProvider")
    execute_provider = generation[execute_provider_at:] if execute_provider_at >= 0 else ""
    narrow_lookup = execute_provider.find("findChatProvider(session.request.api)")
    legacy_lookup = execute_provider.find("findProvider(session.request.api)")
    if narrow_lookup < 0:
        errors.append("GenerationService no longer resolves IChatProvider")
    if legacy_lookup < 0:
        errors.append("GenerationService lost the explicit Retool legacy fallback")
    if narrow_lookup >= 0 and legacy_lookup >= 0 and narrow_lookup > legacy_lookup:
        errors.append("GenerationService must prefer IChatProvider before APIinterface fallback")
    for needle in (
        "ProviderCallContext context{cancellation, deadline}",
        "return result.error()",
        "applyProviderResponse(session, result.value())",
    ):
        if needle not in generation:
            errors.append(f"GenerationService Chayns Result bridge missing: {needle}")

    # Model and upstream-thread ownership are separate capabilities. Session
    # cleanup/rebind and the reaper must not rediscover Chayns through the wide
    # port merely because Retool still uses it.
    if "findModelCatalog(provider)" not in read(SRC / "sessionManager/core/AiApiUseCase.cpp"):
        errors.append("AiApiUseCase does not resolve the narrow model catalog")
    thread_lookup = session.find("findThreadContext(providerName)")
    session_legacy = session.find("findProvider(providerName)")
    if thread_lookup < 0 or (session_legacy >= 0 and thread_lookup > session_legacy):
        errors.append("Session thread cleanup/transfer must prefer IProviderThreadContext")
    if "findThreadContext(\"chaynsapi\")" not in reaper:
        errors.append("chaynsThreadReaper must use IProviderThreadContext")
    if 'findProvider("chaynsapi")' in reaper:
        errors.append("chaynsThreadReaper revived a legacy Chayns lookup")

    for needle in (
        "findChatProvider",
        "findModelCatalog",
        "findThreadContext",
    ):
        if needle not in registry_port:
            errors.append(f"IProviderRegistry narrow capability missing: {needle}")

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

    print("PASS P6-W2: Chayns uses the narrow ProviderBase/Result slice without legacy session fallback")
    return 0


if __name__ == "__main__":
    sys.exit(main())
