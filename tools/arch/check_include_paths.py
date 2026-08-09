#!/usr/bin/env python3
"""P3 include gate: self headers use the single ``src/`` include root."""

from __future__ import annotations

import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc"}
HEADER_SUFFIXES = {".h", ".hpp"}
INCLUDE_RE = re.compile(
    r"^\s*#\s*include\s*(?P<open>[<\"])(?P<path>[^>\"]+)[>\"]", re.MULTILINE
)
TARGET_INCLUDES_RE = re.compile(
    r"target_include_directories\s*\((?P<body>.*?)\)", re.DOTALL
)
CURRENT_SOURCE_RE = re.compile(r"\$\{CMAKE_CURRENT_SOURCE_DIR\}(?P<suffix>/[^\s)#]+)?")


def source_files() -> list[Path]:
    return sorted(
        path
        for path in SRC.rglob("*")
        if path.is_file()
        and path.suffix in SOURCE_SUFFIXES
        and "build" not in path.relative_to(SRC).parts
    )


def header_index() -> tuple[list[Path], dict[str, list[Path]]]:
    headers = [path for path in source_files() if path.suffix in HEADER_SUFFIXES]
    by_name: dict[str, list[Path]] = defaultdict(list)
    for header in headers:
        by_name[header.name].append(header)
    return headers, by_name


def inside_src(path: Path) -> bool:
    try:
        path.resolve().relative_to(SRC.resolve())
        return True
    except ValueError:
        return False


def canonical(path: Path) -> str:
    return path.resolve().relative_to(SRC.resolve()).as_posix()


def classify_include(
    consumer: Path, include: str, headers_by_name: dict[str, list[Path]]
) -> tuple[str, str | None]:
    """Return (category, canonical self path when one can be resolved)."""
    if ".." in Path(include).parts:
        relative = (consumer.parent / include).resolve()
        target = canonical(relative) if relative.is_file() and inside_src(relative) else None
        return "dotdot", target

    from_root = SRC / include
    if from_root.is_file() and from_root.suffix in HEADER_SUFFIXES:
        return "canonical", canonical(from_root)

    relative = consumer.parent / include
    if relative.is_file() and relative.suffix in HEADER_SUFFIXES and inside_src(relative):
        return "relative", canonical(relative)

    matches = headers_by_name.get(Path(include).name, [])
    if len(matches) == 1:
        return "basename", canonical(matches[0])
    if len(matches) > 1:
        return "ambiguous", None
    return "external", None


def cmake_include_roots() -> tuple[int, list[str]]:
    roots = 0
    violations: list[str] = []
    for cmake in (SRC / "CMakeLists.txt", SRC / "test/CMakeLists.txt"):
        body = cmake.read_text(encoding="utf-8")
        base = cmake.parent
        for block in TARGET_INCLUDES_RE.finditer(body):
            for match in CURRENT_SOURCE_RE.finditer(block.group("body")):
                suffix = match.group("suffix") or ""
                resolved = (base / ("." + suffix)).resolve()
                if not inside_src(resolved):
                    continue
                if resolved != SRC.resolve():
                    violations.append(
                        f"{cmake.relative_to(ROOT)} exposes subdirectory include root: "
                        f"{match.group(0)}"
                    )
                else:
                    roots += 1
    return roots, violations


def main() -> int:
    headers, headers_by_name = header_index()
    duplicates = {name: paths for name, paths in headers_by_name.items() if len(paths) > 1}
    counts: Counter[str] = Counter()
    violations: list[str] = []

    for source in source_files():
        body = source.read_text(encoding="utf-8", errors="replace")
        for match in INCLUDE_RE.finditer(body):
            include = match.group("path")
            category, target = classify_include(source, include, headers_by_name)
            counts[category] += 1
            if category == "canonical":
                if match.group("open") != "<":
                    violations.append(
                        f"{source.relative_to(ROOT)} uses quoted self include; "
                        f"expected <{target}>: {include}"
                    )
                continue
            if category in {"external"}:
                continue
            expected = f"; expected <{target}>" if target else ""
            violations.append(
                f"{source.relative_to(ROOT)} has non-canonical self include "
                f"({category}): {include}{expected}"
            )

    root_count, cmake_violations = cmake_include_roots()
    violations.extend(cmake_violations)
    if root_count != 1:
        violations.append(
            f"CMake declares the src include root {root_count} times; expected exactly 1"
        )

    print(
        "include inventory: "
        f"headers={len(headers)}, duplicate_basenames={len(duplicates)}, "
        f"canonical={counts['canonical']}, relative={counts['relative']}, "
        f"basename={counts['basename']}, dotdot={counts['dotdot']}, "
        f"ambiguous={counts['ambiguous']}, external={counts['external']}, "
        f"cmake_src_roots={root_count}"
    )
    if duplicates:
        # Not a permanent naming ban: full paths remain unambiguous.  Report the
        # inventory because zero duplicates is the scripted P3 migration precondition.
        for name, paths in sorted(duplicates.items()):
            rendered = ", ".join(str(path.relative_to(ROOT)) for path in paths)
            print(f"WARN: duplicate self-header basename {name}: {rendered}", file=sys.stderr)

    if violations:
        for violation in violations:
            print(f"FAIL: {violation}", file=sys.stderr)
        return 1

    print(
        "PASS include paths: all self headers use <path/from/src>; "
        "CMake exposes one src include root"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
