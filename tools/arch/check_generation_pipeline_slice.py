#!/usr/bin/env python3
"""P7-W1 ratchet for the GenerationService pipeline split."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
from typing import Mapping


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4

SERVICE_HEADER = SRC / "application/generation/core/GenerationService.h"
SERVICE_CPP = SRC / "application/generation/core/GenerationService.cpp"
PIPELINE_HEADER = SRC / "application/generation/core/GenerationPipeline.h"
PIPELINE_CPP = SRC / "application/generation/core/GenerationPipeline.cpp"
RESPONSE_HEADER = SRC / "application/generation/core/GenerationResponsePipeline.h"
RESPONSE_CPP = SRC / "application/generation/core/GenerationResponsePipeline.cpp"
LEGACY_CPP = SRC / "application/generation/core/GenerationServiceEmitAndToolBridge.cpp"
CMAKE = SRC / "CMakeLists.txt"
FIXTURE_TEST = SRC / "test/test_generation_service_bridge_fixture.cpp"

TOOLING_SOURCES = {
    SRC / "application/generation/tooling/ForcedToolCallGenerator.cpp":
        "void toolcall::generateForcedToolCall(",
    SRC / "application/generation/tooling/ToolCallNormalizer.cpp":
        "void toolcall::normalizeToolCallArguments(",
    SRC / "application/generation/tooling/ToolDefinitionEncoder.cpp":
        "void toolcall::transformRequestForToolBridge(",
}


def uncommented(text: str) -> str:
    """Avoid letting a migration note satisfy or evade a source rule."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def cmake_source_list(cmake: str, name: str) -> str:
    match = re.search(r"set\(\s*" + re.escape(name) + r"\s*(.*?)\n\)",
                      cmake, flags=re.S)
    return "" if match is None else match.group(1)


def validate(overrides: Mapping[Path, str] | None = None) -> list[str]:
    overrides = overrides or {}
    errors: list[str] = []

    def read(path: Path) -> str:
        if path in overrides:
            return overrides[path]
        if not path.is_file():
            errors.append(f"missing required P7-W1 file: {path.relative_to(ROOT)}")
            return ""
        return path.read_text(errors="replace")

    service_header = read(SERVICE_HEADER)
    service_cpp = read(SERVICE_CPP)
    pipeline_header = read(PIPELINE_HEADER)
    pipeline_cpp = read(PIPELINE_CPP)
    response_header = read(RESPONSE_HEADER)
    response_cpp = read(RESPONSE_CPP)
    cmake = read(CMAKE)
    fixture_test = read(FIXTURE_TEST)
    tooling = {path: read(path) for path in TOOLING_SOURCES}

    if LEGACY_CPP.exists():
        errors.append("P7-W1 must delete GenerationServiceEmitAndToolBridge.cpp")

    p7_sources = (
        "application/generation/core/GenerationService.cpp",
        "application/generation/core/GenerationPipeline.cpp",
        "application/generation/core/GenerationResponsePipeline.cpp",
        "application/generation/tooling/ForcedToolCallGenerator.cpp",
        "application/generation/tooling/ToolCallNormalizer.cpp",
        "application/generation/tooling/ToolDefinitionEncoder.cpp",
    )
    for source in p7_sources:
        if source not in cmake:
            errors.append(f"src/CMakeLists.txt must register P7-W1 source: {source}")
    if "GenerationServiceEmitAndToolBridge.cpp" in cmake:
        errors.append("src/CMakeLists.txt revived the deleted GenerationService implementation")
    application_sources = cmake_source_list(cmake, "AIAPI_APPLICATION_SOURCES")
    legacy_sources = cmake_source_list(cmake, "AIAPI_LEGACY_SOURCES")
    for source in p7_sources:
        if source not in application_sources:
            errors.append(f"P7-W1 source must be owned by aiapi_application: {source}")
        if source in legacy_sources:
            errors.append(f"P7-W1 source must not return to aiapi_legacy: {source}")

    clean_header = uncommented(service_header)
    clean_service = uncommented(service_cpp)
    if "std::unique_ptr<generation::GenerationPipeline> pipeline_;" not in clean_header:
        errors.append("GenerationService must hold exactly the GenerationPipeline facade dependency")
    for stale_member in (
        "providerRegistry_",
        "sessionStore_",
        "responseIndex_",
        "executionGate_",
        "channelCatalog_",
    ):
        if stale_member in clean_header:
            errors.append(f"GenerationService facade revived direct collaborator state: {stale_member}")
    if "std::make_unique<generation::GenerationPipeline>(" not in clean_service:
        errors.append("GenerationService constructor must construct GenerationPipeline")
    if not re.search(
        r"GenerationService::runGuarded\s*\([^)]*\)\s*\{\s*"
        r"return\s+pipeline_->run\(request,\s*sink,\s*policy\);\s*\}",
        clean_service,
        flags=re.S,
    ):
        errors.append("GenerationService::runGuarded must remain a direct pipeline delegation")

    stale_service_members = (
        "GenerationService::materializeSession",
        "GenerationService::computeExecutionKey",
        "GenerationService::executeGuardedWithSession",
        "GenerationService::executeProvider",
        "GenerationService::providerRequestFromSession",
        "GenerationService::applyProviderResponse",
        "GenerationService::emitResultEvents",
        "GenerationService::emitError",
        "GenerationService::sanitizeOutput",
        "GenerationService::getChannelSupportsToolCalls",
        "GenerationService::parseXmlToolCalls",
    )
    for stale_member in stale_service_members:
        if stale_member in clean_header or stale_member in clean_service:
            errors.append(f"GenerationService facade revived P7-W1 stage: {stale_member}")

    for needle in (
        "class GenerationPipeline",
        "static session_st materializeRequest",
        "ToolBridgeState prepareToolBridge",
        "retryCodexBridgeResponse",
        "invokeProvider",
    ):
        if needle not in pipeline_header:
            errors.append(f"GenerationPipeline stage contract missing: {needle}")
    for needle in (
        "ContinuityResolver resolver",
        "ExecutionGuard guard",
        "prepareToolBridge(session)",
        "ProviderCallContext context{cancellation, deadline}",
        "retryCodexBridgeResponse(session, bridge, cancellation, deadline)",
        "responsePipeline_.emit(session, sink)",
        "persistCompletedSession(sessionStore, *responseIndex_, session)",
    ):
        if needle not in pipeline_cpp:
            errors.append(f"GenerationPipeline orchestration stage missing: {needle}")

    for needle in (
        "class GenerationResponsePipeline",
        "void emit(const session_st& session, IResponseSink& sink) const",
        "void emitError(const platform::Error& error, IResponseSink& sink) const",
    ):
        if needle not in response_header:
            errors.append(f"GenerationResponsePipeline contract missing: {needle}")
    emit_at = response_cpp.find("GenerationResponsePipeline::emit(")
    response_emit = response_cpp[emit_at:] if emit_at >= 0 else ""
    ordered_response_stages = (
        "extractResponseOutput(session, supportsToolCalls, assembly);",
        "annotateToolCallIdentities(session, assembly.toolCalls);",
        "filterForcedTool(session, assembly);",
        "resolveCodexCompletion(assembly);",
        "toolcall::normalizeToolCallArguments(session, assembly.toolCalls);",
        "validateToolCalls(session, assembly);",
        "applyClientRules(session, assembly);",
        "embedSessionIdIfNeeded(sessionStore_, session, assembly.clientCapabilities, assembly, sink);",
        "emitProtocolEvents(session, assembly, sink);",
    )
    cursor = 0
    for stage in ordered_response_stages:
        position = response_emit.find(stage, cursor)
        if position < 0:
            errors.append(f"GenerationResponsePipeline response stage missing or out of order: {stage}")
            continue
        cursor = position + len(stage)

    for path, definition in TOOLING_SOURCES.items():
        if definition not in tooling[path]:
            errors.append(f"P7-W1 tooling implementation missing: {path.relative_to(ROOT)}")

    for test_name in (
        "GenerationService_ToolBridgeTransformsRequestThroughRunGuarded",
        "GenerationService_BridgeCodecAndEmitOrderRunThroughProductionPipeline",
        "GenerationService_NativeToolArgumentsAreNormalizedBeforeEmit",
        "GenerationService_RequiredToolFallbackRunsInsideEmitResultEvents",
        "GenerationService_ProviderFailurePreservesSemanticErrorAndCloses",
    ):
        if test_name not in fixture_test:
            errors.append(f"P7-W1 production-pipeline coverage missing: {test_name}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="P7-W1 Generation pipeline slice ratchet")
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="prove an in-memory facade delegation mutation is rejected",
    )
    args = parser.parse_args()

    errors = validate()
    if errors:
        print("P7-W1 Generation pipeline slice gate FAIL")
        for error in errors:
            print(f"  {error}")
        return FAIL

    if args.selftest:
        original = SERVICE_CPP.read_text(errors="replace")
        needle = "return pipeline_->run(request, sink, policy);"
        mutation = original.replace(needle, "return std::nullopt;", 1)
        if mutation == original:
            print("P7-W1 Generation pipeline slice gate selftest FAIL")
            print("  probe could not mutate GenerationService facade delegation")
            return FAIL
        mutation_errors = validate({SERVICE_CPP: mutation})
        if not mutation_errors:
            print("P7-W1 Generation pipeline slice gate selftest FAIL")
            print("  facade delegation mutation unexpectedly passed")
            return FAIL
        print("PASS P7-W1 Generation pipeline slice gate selftest: facade mutation was rejected")

    print("PASS P7-W1: GenerationService is a thin facade and generation stages remain split")
    return 0


if __name__ == "__main__":
    sys.exit(main())
