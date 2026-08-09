#!/usr/bin/env python3
"""P3 gate: every production implementation has one compiled production owner."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
SRC_CMAKE = SRC / "CMakeLists.txt"
TEST_CMAKE = SRC / "test/CMakeLists.txt"
SUFFIXES = {".cpp", ".cc"}


def production_files() -> set[Path]:
    return {
        p.resolve()
        for p in SRC.rglob("*")
        if p.is_file()
        and p.suffix in SUFFIXES
        and "test" not in p.relative_to(SRC).parts
        and "build" not in p.relative_to(SRC).parts
    }


def listed_library_sources() -> list[Path]:
    body = SRC_CMAKE.read_text(encoding="utf-8")
    values: list[Path] = []
    pattern = re.compile(r"set\(\s*AIAPI_[A-Z0-9_]+_SOURCES\s(.*?)\)", re.S)
    for match in pattern.finditer(body):
        for raw in match.group(1).splitlines():
            value = raw.split("#", 1)[0].strip()
            if not value or "${" in value or Path(value).suffix not in SUFFIXES:
                continue
            values.append((SRC / value).resolve())
    return values


def fail(messages: list[str]) -> None:
    for message in messages:
        print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compile-commands", type=Path)
    args = parser.parse_args()

    failures: list[str] = []
    files = production_files()
    main_source = (SRC / "main.cc").resolve()
    listed = listed_library_sources()
    owners = Counter(listed + [main_source])

    for path in sorted(files):
        count = owners[path]
        if count != 1:
            failures.append(
                f"production source owner count is {count}, expected 1: {path.relative_to(ROOT)}")
    for path in owners:
        if path not in files:
            failures.append(f"CMake source owner points outside production set: {path}")

    test_cmake = TEST_CMAKE.read_text(encoding="utf-8")
    if "PROJECT_SOURCES" in test_cmake:
        failures.append("src/test/CMakeLists.txt still declares PROJECT_SOURCES")
    if re.search(r"\.\./[^\s)]*\.(?:cpp|cc)\b", test_cmake):
        failures.append("test target still compiles a production implementation path")
    if "target_link_libraries(${PROJECT_NAME} PRIVATE aiapi_legacy" not in test_cmake:
        failures.append("aiapi_test does not link the canonical production library")

    if args.compile_commands:
        path = args.compile_commands.resolve()
        if not path.is_file():
            failures.append(f"compile_commands.json not found: {path}")
        else:
            commands = json.loads(path.read_text(encoding="utf-8"))
            compiled = Counter(Path(item["file"]).resolve() for item in commands)
            for source in sorted(files):
                count = compiled[source]
                if count != 1:
                    failures.append(
                        f"compile database count is {count}, expected 1: {source.relative_to(ROOT)}")

    if failures:
        fail(failures)

    print(
        "PASS source ownership: "
        f"{len(files)} production implementations, one owner/compile each; tests link aiapi_legacy")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
