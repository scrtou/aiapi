#!/usr/bin/env python3
"""Enforce the final ADR-02 CMake target DAG and absence of legacy owners."""

from __future__ import annotations

import argparse
import re
import shlex
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CMAKE = ROOT / "src/CMakeLists.txt"

TARGET_KINDS = {
    "aiapi_platform": {"STATIC"},
    # P3-W4 will let these header-only targets become compiled libraries.
    "aiapi_domain": {"INTERFACE", "STATIC"},
    "aiapi_application": {"INTERFACE", "STATIC"},
    "aiapi_infrastructure": {"STATIC"},
    "aiapi_transport": {"STATIC"},
    "aiapi_runtime": {"STATIC"},
}

ALLOWED_INTERNAL_LINKS = {
    "aiapi_platform": set(),
    "aiapi_domain": {"aiapi_platform"},
    "aiapi_application": {"aiapi_domain", "aiapi_platform"},
    "aiapi_infrastructure": {"aiapi_domain", "aiapi_platform"},
    "aiapi_transport": {
        "aiapi_application",
        "aiapi_domain",
        "aiapi_platform",
    },
    "aiapi_runtime": {
        "aiapi_application",
        "aiapi_infrastructure",
        "aiapi_transport",
    },
}

SOURCE_LISTS = {
    "aiapi_platform": "AIAPI_PLATFORM_SOURCES",
    "aiapi_application": "AIAPI_APPLICATION_SOURCES",
    "aiapi_infrastructure": "AIAPI_INFRASTRUCTURE_SOURCES",
    "aiapi_transport": "AIAPI_TRANSPORT_SOURCES",
    "aiapi_runtime": "AIAPI_RUNTIME_SOURCES",
}


def command_bodies(text: str, command: str) -> list[str]:
    """Return balanced bodies for simple CMake commands."""
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


def tokens(body: str) -> list[str]:
    without_comments = "\n".join(line.split("#", 1)[0] for line in body.splitlines())
    return shlex.split(without_comments, posix=True)


def variable_values(text: str, name: str) -> list[str] | None:
    for body in command_bodies(text, "set"):
        values = tokens(body)
        if values and values[0] == name:
            return values[1:]
    return None


def fail(messages: list[str]) -> None:
    for message in messages:
        print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake-file", type=Path, default=DEFAULT_CMAKE)
    parser.add_argument(
        "--require-no-legacy",
        action="store_true",
        help="assert the final no-legacy invariant (now the default)",
    )
    args = parser.parse_args()

    text = args.cmake_file.read_text(encoding="utf-8")
    failures: list[str] = []

    declared: dict[str, str] = {}
    for body in command_bodies(text, "add_library"):
        values = tokens(body)
        if len(values) >= 2:
            declared[values[0]] = values[1]
    for target, expected_kinds in TARGET_KINDS.items():
        actual = declared.get(target)
        if actual not in expected_kinds:
            failures.append(
                f"{target} kind is {actual or 'missing'}, expected one of {sorted(expected_kinds)}")

    links = {target: set() for target in TARGET_KINDS}
    for body in command_bodies(text, "target_link_libraries"):
        values = tokens(body)
        if not values or values[0] not in links:
            continue
        for value in values[1:]:
            if value in TARGET_KINDS:
                links[values[0]].add(value)
    for target, allowed in ALLOWED_INTERNAL_LINKS.items():
        unexpected = links[target] - allowed
        missing = allowed - links[target]
        if unexpected:
            failures.append(
                f"{target} has forbidden internal link(s): {sorted(unexpected)}")
        if missing:
            failures.append(
                f"{target} is missing declared DAG link(s): {sorted(missing)}")

    registrations: dict[str, set[str]] = {}
    for body in command_bodies(text, "aiapi_register_production_sources"):
        values = tokens(body)
        if values:
            registrations.setdefault(values[0], set()).update(values[1:])
    for target, source_list in SOURCE_LISTS.items():
        expected = {"${" + source_list + "}"}
        if registrations.get(target, set()) != expected:
            failures.append(
                f"{target} must register exactly ${{{source_list}}}")

    # Drogon HttpController registration is a static initializer.  Once a
    # controller implementation moves to aiapi_transport there is no normal
    # undefined-symbol edge that would extract its object from a static archive.
    # Production must retain transport; tests intentionally must not.
    retained_targets = r"aiapi_transport"
    whole_archive_pattern = re.compile(
        r'"-Wl,--whole-archive"\s+' + retained_targets +
        r'\s+"-Wl,--no-whole-archive"')
    if not whole_archive_pattern.search(text):
        failures.append(
            "production whole-archive segment does not retain required registration target(s)")

    legacy_values = variable_values(text, "AIAPI_LEGACY_SOURCES")
    legacy_declared = "aiapi_legacy" in declared
    if legacy_values is not None:
        failures.append("AIAPI_LEGACY_SOURCES still exists")
    if legacy_declared:
        failures.append("aiapi_legacy target still exists")

    if failures:
        fail(failures)

    print("PASS target layers: six formal targets obey ADR-02 DAG; legacy_sources=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
