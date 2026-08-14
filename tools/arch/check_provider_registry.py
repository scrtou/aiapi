#!/usr/bin/env python3
"""P5-W1 gate: legacy provider service locators must stay deleted."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4

errors = []
for rel in (
    "apiManager/ApiFactory.h",
    "apiManager/ApiFactory.cpp",
    "apiManager/ApiManager.h",
    "apiManager/ApiManager.cpp",
):
    if (SRC / rel).exists():
        errors.append(f"legacy service locator still exists: src/{rel}")

pattern = re.compile(r"\b(?:ApiFactory|ApiManager)\b|\b(?:DEClARE|IMPLEMENT)_RUNTIME\b")
for path in SRC.rglob("*"):
    if path.suffix not in {".h", ".hpp", ".cpp", ".cc"} or "test" in path.parts:
        continue
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if pattern.search(line):
            errors.append(f"{path.relative_to(ROOT)}:{number}: {line.strip()}")

wiring = (SRC / "runtime/AppWiring.cpp").read_text(errors="replace")
required = (
    'registerProvider("chaynsapi"',
    'registerProvider("retoolapi"',
    "registry->freeze()",
    "setProviderRegistry(registry.get())",
)
for needle in required:
    if needle not in wiring:
        errors.append(f"runtime provider wiring missing: {needle}")

if errors:
    print("P5-W1 ProviderRegistry gate FAIL")
    for error in errors:
        print(f"  {error}")
    raise SystemExit(FAIL)

print("PASS P5-W1: ApiFactory/ApiManager deleted; providers are explicitly registered and frozen")
