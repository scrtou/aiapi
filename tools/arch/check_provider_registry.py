#!/usr/bin/env python3
"""P5/P6 registry ratchet: no provider service locator or legacy lane."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4

errors: list[str] = []
for rel in (
    "apiManager/ApiFactory.h",
    "apiManager/ApiFactory.cpp",
    "apiManager/ApiManager.h",
    "apiManager/ApiManager.cpp",
    "domain/port/APIinterface.h",
):
    if (SRC / rel).exists():
        errors.append(f"legacy provider service/port still exists: src/{rel}")

locator_pattern = re.compile(r"\b(?:ApiFactory|ApiManager)\b|\b(?:DEClARE|IMPLEMENT)_RUNTIME\b")
legacy_registry_pattern = re.compile(r"\b(?:APIinterface|findProvider|registerProvider)\b")
for path in SRC.rglob("*"):
    if path.suffix not in {".h", ".hpp", ".cpp", ".cc"} or "test" in path.parts:
        continue
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if locator_pattern.search(line):
            errors.append(f"{path.relative_to(ROOT)}:{number}: {line.strip()}")
        if legacy_registry_pattern.search(line):
            errors.append(
                f"{path.relative_to(ROOT)}:{number}: legacy ProviderRegistry lane: {line.strip()}"
            )

registry_port = SRC / "domain/port/IProviderRegistry.h"
registry_impl = SRC / "infrastructure/provider/ProviderRegistry.h"
for path in (registry_port, registry_impl):
    if not path.is_file():
        errors.append(f"missing narrow provider registry file: {path.relative_to(ROOT)}")
        continue
    code = path.read_text(errors="replace")
    if "findChatProvider" not in code:
        errors.append(f"narrow provider registry capability missing: {path.relative_to(ROOT)}")
    if re.search(r"\b(?:APIinterface|findProvider|registerProvider)\b", code):
        errors.append(f"legacy provider registry declaration revived: {path.relative_to(ROOT)}")

wiring = (SRC / "runtime/AppWiring.cpp").read_text(errors="replace")
required = {
    "Chayns narrow registration": r'registerChatProvider\s*\(\s*"chaynsapi"',
    "Retool narrow registration": r'registerChatProvider\s*\(\s*"retoolapi"',
    "registry freeze": r"registry->freeze\s*\(\s*\)",
    "registry publication": r"setProviderRegistry\s*\(\s*registry\.get\s*\(\s*\)\s*\)",
}
for label, pattern in required.items():
    if not re.search(pattern, wiring):
        errors.append(f"runtime provider wiring missing: {label}")
if re.search(r"\bregisterProvider\s*\(", wiring):
    errors.append("runtime provider wiring revived legacy registerProvider()")

if errors:
    print("P5/P6 ProviderRegistry gate FAIL")
    for error in errors:
        print(f"  {error}")
    raise SystemExit(FAIL)

print("PASS P5/P6: ApiFactory/ApiManager/APIinterface and the legacy registry lane are deleted; active providers are explicitly narrow-registered and frozen")
