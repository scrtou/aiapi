#!/usr/bin/env python3
"""P5-W3: migrated session application paths use injected ports only."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4
errors = []

paths = [
    "application/generation/continuity/ResponseIndex.cpp",
    "application/generation/core/Session.cpp",
    "application/generation/core/RequestAdapters.cpp",
    "application/generation/core/GenerationService.cpp",
    "application/generation/core/GenerationPipeline.cpp",
    "application/generation/core/GenerationResponsePipeline.cpp",
    "application/generation/core/AiApiUseCase.cpp",
    "application/generation/core/RetiredProviderTelemetry.cpp",
    "application/generation/tooling/BridgeHelpers.cpp",
    "application/generation/tooling/ForcedToolCallGenerator.cpp",
    "application/generation/tooling/ToolCallNormalizer.cpp",
    "application/generation/tooling/ToolDefinitionEncoder.cpp",
]
for relative in paths:
    text = (SRC / relative).read_text(errors="replace")
    for match in re.finditer(r"::\s*(?:getInstance|instance)\s*\(", text):
        line = text.count("\n", 0, match.start()) + 1
        errors.append(f"{relative}:{line}: session application path locates a service")
    for match in re.finditer(
        r"#\s*include\s*[<\"](?:accountManager|channelManager|dbManager|metrics)/",
        text,
    ):
        line = text.count("\n", 0, match.start()) + 1
        errors.append(f"{relative}:{line}: session path includes a concrete service")

wiring = (SRC / "runtime/AppWiring.cpp").read_text(errors="replace")
for needle in (
    "std::make_shared<ResponseIndex>(sessionDb.get())",
    "sessionStore->setPersistence(sessionDb.get())",
    "RequestAdapters::setAccountSettingsQuery(accounts.get())",
    "std::make_shared<AiApiUseCase>(",
    "ctx.setAiApiUseCase(aiApiUseCase)",
    "ctx.setErrorStatsService(errorStats)",
    "bridge::setTelemetrySink(errorStats.get())",
    "observability::setTelemetrySink(errorStats.get())",
):
    if needle not in wiring:
        errors.append(f"runtime session application wiring missing: {needle}")

if errors:
    print("P5-W3 session application service injection gate FAIL")
    for error in errors:
        print(f"  {error}")
    raise SystemExit(FAIL)

print("PASS P5-W3: session application paths use injected persistence/catalog/telemetry ports")
