#!/usr/bin/env python3
"""P8-W2: keep physical src/ layout aligned with the six formal targets.

Target ownership alone does not prevent a source from remaining in a historical
``sessionManager/`` or ``dbManager/`` directory.  This gate makes the target
file tree executable: each implementation list must live below the matching
layer directory and no retired top-level source directory may reappear.
"""

from __future__ import annotations

import argparse
import re
import shlex
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
CMAKE = SRC / "CMakeLists.txt"
FAIL = 4

SOURCE_LISTS = {
    "AIAPI_PLATFORM_SOURCES": "platform",
    "AIAPI_APPLICATION_SOURCES": "application",
    "AIAPI_INFRASTRUCTURE_SOURCES": "infrastructure",
    "AIAPI_TRANSPORT_SOURCES": "transport",
    "AIAPI_RUNTIME_SOURCES": "runtime",
}
ALLOWED_TOP_LEVEL_DIRECTORIES = {
    "application",
    "domain",
    "infrastructure",
    "platform",
    "runtime",
    "test",
    "transport",
}
LEGACY_TOP_LEVEL_DIRECTORIES = {
    "accountManager",
    "apiManager",
    "channelManager",
    "controllers",
    "dbManager",
    "managedAccount",
    "metrics",
    "models",
    "retoolWorkspace",
    "sessionManager",
}
SOURCE_SUFFIXES = {".cpp", ".cc"}


def command_bodies(text: str, command: str) -> list[str]:
    """Read balanced, non-nested CMake command bodies."""
    bodies: list[str] = []
    pattern = re.compile(r"(?im)^\s*" + re.escape(command) + r"\s*\(")
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
            elif not quote and char == "(":
                depth += 1
            elif not quote and char == ")":
                depth -= 1
            index += 1
        if depth:
            raise ValueError(f"unterminated {command}(...) command")
        bodies.append(text[match.end():index - 1])
    return bodies


def cmake_tokens(body: str) -> list[str]:
    uncommented = "\n".join(line.split("#", 1)[0] for line in body.splitlines())
    return shlex.split(uncommented, posix=True)


def source_lists(cmake_text: str) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for body in command_bodies(cmake_text, "set"):
        values = cmake_tokens(body)
        if not values or values[0] not in SOURCE_LISTS:
            continue
        result[values[0]] = [
            value for value in values[1:] if Path(value).suffix in SOURCE_SUFFIXES
        ]
    return result


def validate(
    cmake_text: str | None = None,
    top_level_directories: set[str] | None = None,
) -> list[str]:
    errors: list[str] = []
    cmake_text = CMAKE.read_text(encoding="utf-8") if cmake_text is None else cmake_text

    current_directories = (
        {path.name for path in SRC.iterdir() if path.is_dir()}
        if top_level_directories is None
        else top_level_directories
    )
    unexpected = sorted(current_directories - ALLOWED_TOP_LEVEL_DIRECTORIES)
    if unexpected:
        errors.append(
            "unexpected top-level src directories: " + ", ".join(unexpected))
    missing = sorted(ALLOWED_TOP_LEVEL_DIRECTORIES - current_directories)
    if missing:
        errors.append(
            "required top-level src directories are missing: " + ", ".join(missing))
    revived = sorted(current_directories & LEGACY_TOP_LEVEL_DIRECTORIES)
    if revived:
        errors.append(
            "legacy top-level source directories revived: " + ", ".join(revived))

    for required in ("CMakeLists.txt", "main.cc"):
        if not (SRC / required).is_file():
            errors.append(f"required src root file is missing: src/{required}")

    try:
        lists = source_lists(cmake_text)
    except ValueError as exc:
        return [str(exc)]

    implementation_count = 0
    for list_name, expected_root in SOURCE_LISTS.items():
        entries = lists.get(list_name)
        if entries is None:
            errors.append(f"missing CMake source list: {list_name}")
            continue
        if not entries:
            errors.append(f"CMake source list is empty: {list_name}")
        for entry in entries:
            implementation_count += 1
            path = Path(entry)
            if path.parts[0] != expected_root:
                errors.append(
                    f"{list_name} source is outside src/{expected_root}/: {entry}")
            if not (SRC / path).is_file():
                errors.append(f"CMake source does not exist: src/{entry}")

    # This is intentionally independent of source-ownership's full scan.  It
    # protects the directory-to-target invariant; the ownership gate protects
    # cardinality and compile-command evidence.
    if implementation_count == 0:
        errors.append("no formal production implementations were discovered")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="validate P8-W2 physical source layout")
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="prove that an in-memory wrong-layer CMake source is rejected",
    )
    args = parser.parse_args()

    errors = validate()
    if errors:
        print("P8-W2 physical layout gate FAIL")
        for error in errors:
            print(f"  {error}")
        return FAIL

    if args.selftest:
        original = CMAKE.read_text(encoding="utf-8")
        needle = "application/account/accountManager.cpp"
        if needle not in original:
            print("P8-W2 physical layout gate selftest FAIL")
            print(f"  fixture source not found in CMake: {needle}")
            return FAIL
        mutated = original.replace(
            needle, "infrastructure/account/accountManager.cpp", 1)
        if not validate(mutated):
            print("P8-W2 physical layout gate selftest FAIL")
            print("  wrong-layer CMake source unexpectedly passed")
            return FAIL

        current_directories = {path.name for path in SRC.iterdir() if path.is_dir()}
        revived = validate(top_level_directories=current_directories | {"sessionManager"})
        if not revived:
            print("P8-W2 physical layout gate selftest FAIL")
            print("  revived legacy directory unexpectedly passed")
            return FAIL
        print(
            "PASS P8-W2 physical layout gate selftest: wrong-layer source and "
            "legacy directory were rejected")

    print(
        "PASS P8-W2 physical layout: formal source owners live below their "
        "matching layer; legacy root directories=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
