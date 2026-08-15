#!/usr/bin/env python3
"""ADR-09 gate: application/domain code must not own HTTP, DB, or sleeps.

The CMake target DAG prevents an application *link* to Drogon.  That is not
enough on its own because all targets intentionally share the canonical src/
include root.  This gate follows the local include closure of the sources in
AIAPI_APPLICATION_SOURCES, scans every domain header, and keeps framework IO
and uninterruptible waits on the infrastructure/transport side of the seam.
"""

from __future__ import annotations

import argparse
import re
import shlex
import sys
from pathlib import Path
from typing import Mapping


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
CMAKE = SRC / "CMakeLists.txt"
FAIL = 4

REQUEST_ADAPTERS_H = SRC / "application/generation/core/RequestAdapters.h"
REQUEST_ADAPTERS_CPP = SRC / "application/generation/core/RequestAdapters.cpp"
ACCOUNT_HTTP_PORT = SRC / "domain/port/IAccountHttpTransport.h"
ACCOUNT_HTTP_ADAPTER = SRC / "infrastructure/account/DrogonAccountHttpTransport.cpp"
WIRING = SRC / "runtime/AppWiring.cpp"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
SOURCE_SUFFIXES = {".cpp", ".cc"}
HEADER_SUFFIXES = {".h", ".hpp", ".hh", ".hxx"}

# These are implementation symbols, not prose.  validate() strips comments
# first so ADR text cannot produce a false alarm or satisfy an assertion.
FORBIDDEN = (
    ("Drogon/Trantor include", re.compile(r'^\s*#\s*include\s*[<"](?:drogon|trantor)/', re.M)),
    ("Drogon symbol", re.compile(r'\bdrogon\s*::')),
    ("synchronous HttpClient", re.compile(r'\bHttpClient\b')),
    ("database client", re.compile(r'\bDbClient\b')),
    ("direct std::this_thread::sleep_for", re.compile(r'\bstd\s*::\s*this_thread\s*::\s*sleep_for\s*\(')),
)


def uncommented(text: str) -> str:
    """Remove comments while preserving newlines for useful line diagnostics."""
    without_blocks = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', without_blocks)


def command_bodies(text: str, command: str) -> list[str]:
    """Return balanced CMake command bodies (same deliberately small grammar as target gate)."""
    bodies: list[str] = []
    pattern = re.compile(r'(?im)^\s*' + re.escape(command) + r'\s*\(')
    for match in pattern.finditer(text):
        depth = 1
        index = match.end()
        quote = False
        escaped = False
        while index < len(text) and depth:
            char = text[index]
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quote = not quote
            elif not quote and char == '(':
                depth += 1
            elif not quote and char == ')':
                depth -= 1
            index += 1
        if depth:
            raise ValueError(f"unterminated {command}(...) command")
        bodies.append(text[match.end():index - 1])
    return bodies


def cmake_tokens(body: str) -> list[str]:
    clean = "\n".join(line.split("#", 1)[0] for line in body.splitlines())
    return shlex.split(clean, posix=True)


def application_sources(cmake_text: str) -> list[Path]:
    for body in command_bodies(cmake_text, "set"):
        values = cmake_tokens(body)
        if not values or values[0] != "AIAPI_APPLICATION_SOURCES":
            continue
        sources = [SRC / value for value in values[1:] if Path(value).suffix in SOURCE_SUFFIXES]
        if not sources:
            raise ValueError("AIAPI_APPLICATION_SOURCES is empty")
        return sources
    raise ValueError("missing AIAPI_APPLICATION_SOURCES")


def first_line(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def local_include_closure(
    roots: list[Path],
    reader,
) -> set[Path]:
    """Follow canonical src-relative includes plus each implementation's header."""
    pending = list(roots)
    for source in roots:
        for suffix in HEADER_SUFFIXES:
            sibling = source.with_suffix(suffix)
            if sibling.is_file():
                pending.append(sibling)

    seen: set[Path] = set()
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        text = reader(path)
        for include in INCLUDE_RE.findall(uncommented(text)):
            candidate = SRC / include
            if candidate.is_file():
                pending.append(candidate)
    return seen


def validate(overrides: Mapping[Path, str] | None = None) -> list[str]:
    overrides = overrides or {}
    errors: list[str] = []

    def read(path: Path) -> str:
        if path in overrides:
            return overrides[path]
        if not path.is_file():
            errors.append(f"missing required ADR-09 file: {path.relative_to(ROOT)}")
            return ""
        return path.read_text(encoding="utf-8", errors="replace")

    cmake_text = read(CMAKE)
    try:
        app_sources = application_sources(cmake_text)
    except ValueError as exc:
        errors.append(str(exc))
        app_sources = []

    for source in app_sources:
        if not source.is_file():
            errors.append(f"application source listed by CMake is missing: {source.relative_to(ROOT)}")

    # The application closure is a stronger check than only scanning .cpp
    # files: a Drogon pointer smuggled into an application header is just as
    # much a boundary violation even if the declaration is not called today.
    app_closure = local_include_closure(app_sources, read) if app_sources else set()
    domain_files = {
        path for path in SRC.joinpath("domain").rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES | HEADER_SUFFIXES
    }
    scan_files = app_closure | domain_files
    for path in sorted(scan_files):
        code = uncommented(read(path))
        for label, pattern in FORBIDDEN:
            match = pattern.search(code)
            if match:
                errors.append(
                    f"{path.relative_to(ROOT)}:{first_line(code, match.start())}: "
                    f"ADR-09 application/domain boundary contains {label}")

    # target_link_libraries(aiapi_application ...) must remain free of
    # Drogon's usage requirements even when all source scans happen to pass.
    app_link_bodies = [
        cmake_tokens(body)
        for body in command_bodies(cmake_text, "target_link_libraries")
        if cmake_tokens(body) and cmake_tokens(body)[0] == "aiapi_application"
    ]
    if not app_link_bodies:
        errors.append("missing target_link_libraries(aiapi_application ...)")
    elif any("Drogon::Drogon" in values for values in app_link_bodies):
        errors.append("aiapi_application must not link Drogon::Drogon")

    adapters_h = uncommented(read(REQUEST_ADAPTERS_H))
    adapters_cpp = uncommented(read(REQUEST_ADAPTERS_CPP))
    if re.search(r'\b(?:drogon\s*::\s*)?HttpRequestPtr\b', adapters_h + adapters_cpp):
        errors.append("RequestAdapters must receive copied Json::Value and RequestHeaders, not HttpRequestPtr")
    for method in ("buildGenerationRequestFromChat", "buildGenerationRequestFromResponses"):
        signature = re.compile(
            re.escape(method) +
            r'\s*\(\s*const\s+Json::Value\s*&\s*\w+\s*,\s*'
            r'const\s+aiapi::RequestHeaders\s*&\s*\w+\s*\)',
            re.S,
        )
        if not signature.search(adapters_h):
            errors.append(f"RequestAdapters::{method} must use Json::Value + copied RequestHeaders")

    account_port = uncommented(read(ACCOUNT_HTTP_PORT))
    if any(pattern.search(account_port) for _, pattern in FORBIDDEN):
        errors.append("IAccountHttpTransport must remain framework/IO-client free")
    account_adapter = uncommented(read(ACCOUNT_HTTP_ADAPTER))
    if not re.search(r'#\s*include\s*[<"]drogon/drogon\.h[>"]', account_adapter):
        errors.append("Drogon account HTTP adapter must remain owned by infrastructure")

    wiring = uncommented(read(WIRING))
    for needle in (
        "StartupResult stepInjectStores(AppContext& ctx, const Json::Value& runtimeConfig)",
        "accounts->setRuntimeConfig(runtimeConfig)",
        "StartupResult stepAiApiUseCase(AppContext& ctx, const Json::Value& runtimeConfig)",
        "channels.get(), executor, runtimeConfig)",
        "return stepInjectStores(ctx, customConfig);",
        "return stepAiApiUseCase(ctx, customConfig);",
    ):
        if needle not in wiring:
            errors.append(f"AppWiring must inject runtime configuration at ADR-09 boundary: {needle}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="ADR-09 HTTP/IO application boundary gate")
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="prove an in-memory Drogon include mutation is rejected",
    )
    args = parser.parse_args()

    errors = validate()
    if errors:
        print("ADR-09 HTTP/IO boundary gate FAIL")
        for error in errors:
            print(f"  {error}")
        return FAIL

    if args.selftest:
        original = REQUEST_ADAPTERS_CPP.read_text(encoding="utf-8", errors="replace")
        mutation = '#include <drogon/drogon.h>\n' + original
        mutation_errors = validate({REQUEST_ADAPTERS_CPP: mutation})
        if not mutation_errors:
            print("ADR-09 HTTP/IO boundary gate selftest FAIL")
            print("  injected Drogon include unexpectedly passed")
            return FAIL
        print("PASS ADR-09 HTTP/IO boundary gate selftest: application Drogon mutation was rejected")

    print("PASS ADR-09: application/domain have no direct Drogon, HTTP/DB client, or sleep_for boundary")
    return 0


if __name__ == "__main__":
    sys.exit(main())
