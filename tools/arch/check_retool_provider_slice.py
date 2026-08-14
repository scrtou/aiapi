#!/usr/bin/env python3
"""P6-W3 ratchet for the Retool Provider vertical slice."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
from typing import Mapping


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4

RETOOL_HEADER = SRC / "apipoint/retoolapi/retoolapi.h"
RETOOL_CPP = SRC / "apipoint/retoolapi/retoolapi.cpp"
WIRING = SRC / "runtime/AppWiring.cpp"
GENERATION = SRC / "sessionManager/core/GenerationService.cpp"
REGISTRY_PORT = SRC / "domain/port/IProviderRegistry.h"
REGISTRY_HEADER = SRC / "infrastructure/provider/ProviderRegistry.h"
REGISTRY_CPP = SRC / "infrastructure/provider/ProviderRegistry.cpp"
FIXTURE_TEST = SRC / "test/test_retool_provider_fixture.cpp"
REGISTRY_TEST = SRC / "test/test_provider_registry_port.cpp"
LEGACY_PORT = SRC / "domain/port/APIinterface.h"


def uncommented(text: str) -> str:
    """Remove comments so migration prose cannot satisfy or evade a rule."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def validate(overrides: Mapping[Path, str] | None = None) -> list[str]:
    overrides = overrides or {}
    errors: list[str] = []

    def read(path: Path) -> str:
        if path in overrides:
            return overrides[path]
        if not path.is_file():
            errors.append(f"missing required P6-W3 file: {path.relative_to(ROOT)}")
            return ""
        return path.read_text(errors="replace")

    header = read(RETOOL_HEADER)
    retool_cpp = read(RETOOL_CPP)
    wiring = read(WIRING)
    generation = read(GENERATION)
    registry_port = read(REGISTRY_PORT)
    registry_header = read(REGISTRY_HEADER)
    registry_cpp = read(REGISTRY_CPP)
    fixture_test = read(FIXTURE_TEST)
    registry_test = read(REGISTRY_TEST)

    if LEGACY_PORT.exists():
        errors.append("src/domain/port/APIinterface.h must be deleted after P6-W3")

    # Retool itself must receive only value DTOs plus read-only controls.  Its
    # workflow/agent state stays private and may not rediscover global owners.
    provider_sources: list[Path] = []
    retool_dir = SRC / "apipoint/retoolapi"
    if retool_dir.is_dir():
        for path in sorted(retool_dir.rglob("*")):
            if path.suffix in {".h", ".hpp", ".cpp", ".cc"}:
                provider_sources.append(path)
    else:
        errors.append("missing src/apipoint/retoolapi directory")

    forbidden_literals = (
        "session_st",
        "APIinterface",
        "Session.h",
        "session.response",
    )
    legacy_result = re.compile(r"\bprovider::Provider(?:Result|Error)\b")
    singleton = re.compile(r"::\s*(?:getInstance|instance)\s*\(")
    for path in provider_sources:
        code = uncommented(read(path))
        relative = path.relative_to(ROOT)
        for forbidden in forbidden_literals:
            if forbidden in code:
                errors.append(f"{relative} revived legacy Retool dependency: {forbidden}")
        if legacy_result.search(code):
            errors.append(f"{relative} revived legacy ProviderResult/ProviderError")
        if singleton.search(code):
            errors.append(f"{relative} revived a project singleton lookup")

    if not re.search(
        r"class\s+retoolapi\s+final\s*:\s*public\s+provider::ProviderBase",
        header,
    ):
        errors.append("retoolapi must directly derive from provider::ProviderBase")
    for needle in (
        "public provider::IProviderModelCatalog",
        "public provider::IProviderThreadContext",
        "platform::Result<void> initialize()",
        "doGenerate(",
        "sendWithinContext(",
        "sleepWithinContext(",
        "eraseThreadContext(",
        "transferThreadContext(",
        "deleteUpstreamThread(",
    ):
        if needle not in header:
            errors.append(f"Retool P6-W3 provider contract missing: {needle}")
    for needle in (
        "provider::ProviderRequest",
        "provider::ProviderCallContext",
        "platform::Result<provider::ProviderResponse>",
        "interruptionError(context)",
        "sendWithinContext(",
        "sleepWithinContext(",
        "requestWorkflow(",
        "requestAgent(",
        "routingHints",
    ):
        if needle not in retool_cpp:
            errors.append(f"Retool P6-W3 implementation missing: {needle}")

    # The one registry has only narrow chat/model/thread capabilities now.
    for path, code in ((REGISTRY_PORT, registry_port),
                       (REGISTRY_HEADER, registry_header),
                       (REGISTRY_CPP, registry_cpp)):
        clean = uncommented(code)
        if re.search(r"\b(?:APIinterface|findProvider|registerProvider|legacyProviders_)\b", clean):
            errors.append(f"legacy ProviderRegistry lane revived: {path.relative_to(ROOT)}")
    for needle in ("findChatProvider", "findModelCatalog", "findThreadContext"):
        if needle not in registry_port:
            errors.append(f"IProviderRegistry narrow capability missing: {needle}")

    execute_provider_at = generation.find("GenerationService::executeProvider")
    execute_provider = generation[execute_provider_at:] if execute_provider_at >= 0 else ""
    for needle in (
        "providerRequestFromSession",
        "routingHints.emplace",
        "findChatProvider(session.request.api)",
        "ProviderCallContext context{cancellation, deadline}",
        "applyProviderResponse(session, result.value())",
    ):
        if needle not in generation:
            errors.append(f"GenerationService Retool bridge missing: {needle}")
    if "findProvider(" in uncommented(execute_provider):
        errors.append("GenerationService revived a Retool legacy fallback")

    if "provider::makeProductionProvider<retoolapi>" not in wiring:
        errors.append("runtime Retool slice wiring missing production provider factory")
    if "retoolProvider->initialize()" not in wiring:
        errors.append("runtime Retool slice wiring missing initialize() Result check")
    if not re.search(r'registerChatProvider\s*\(\s*"retoolapi"', wiring):
        errors.append("runtime Retool slice wiring missing narrow registration")
    if re.search(r"\bregisterProvider\s*\(", uncommented(wiring)):
        errors.append("runtime Retool slice wiring revived legacy registration")

    # Fixture coverage must exercise the real adapter's two protocol branches,
    # cancellation boundary and newly exposed narrow capabilities.
    for needle in (
        "RetoolProvider_WorkflowFixtureRunsRealRequestWorkflowOffline",
        "RetoolProvider_AgentFixtureRunsRealRequestAgentOffline",
        "RetoolProvider_CancellationStopsBeforeTheNextPollingBoundary",
        "RetoolProvider_ProvidesNarrowModelAndThreadCapabilities",
    ):
        if needle not in fixture_test:
            errors.append(f"Retool P6-W3 fixture coverage missing: {needle}")
    if "ProviderRegistryPort_ResolvesNarrowCapabilitiesWithoutLegacyFallback" not in registry_test:
        errors.append("P6-W3 registry coverage missing narrow capability assertion")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="P6-W3 Retool provider slice ratchet")
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="also prove the gate detects an in-memory ProviderBase mutation",
    )
    args = parser.parse_args()

    errors = validate()
    if errors:
        print("P6-W3 Retool Provider slice gate FAIL")
        for error in errors:
            print(f"  {error}")
        return FAIL

    if args.selftest:
        original = RETOOL_HEADER.read_text(errors="replace")
        mutation = original.replace("public provider::ProviderBase", "public BrokenProviderBase", 1)
        if mutation == original:
            print("P6-W3 Retool Provider slice gate selftest FAIL")
            print("  probe could not mutate retoolapi ProviderBase inheritance")
            return FAIL
        mutation_errors = validate({RETOOL_HEADER: mutation})
        if not mutation_errors:
            print("P6-W3 Retool Provider slice gate selftest FAIL")
            print("  inheritance mutation unexpectedly passed")
            return FAIL
        print("PASS P6-W3 Retool Provider slice gate selftest: inheritance mutation was rejected")

    print("PASS P6-W3: Retool uses the narrow ProviderBase/Result slice; the legacy provider lane is deleted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
