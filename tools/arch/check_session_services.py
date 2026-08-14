#!/usr/bin/env python3
"""P5-W2 gate: application session services must be injected, not located."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4
errors = []

for path in SRC.rglob("*"):
    if path.suffix not in {".h", ".hpp", ".cpp", ".cc"} or "test" in path.parts:
        continue
    relative = path.relative_to(SRC).as_posix()
    text = path.read_text(errors="replace")
    for pattern, label in (
        (r"ResponseIndex\s*::\s*instance\s*\(", "ResponseIndex::instance"),
        (r"SessionExecutionGate\s*::\s*getInstance\s*\(", "SessionExecutionGate::getInstance"),
    ):
        for match in re.finditer(pattern, text):
            line = text.count("\n", 0, match.start()) + 1
            errors.append(f"{relative}:{line}: forbidden {label}")

    if relative.startswith(("sessionManager/", "controllers/")):
        for match in re.finditer(r"chatSession\s*::\s*getInstance\s*\(", text):
            line = text.count("\n", 0, match.start()) + 1
            errors.append(f"{relative}:{line}: application/transport locates chatSession")

wiring = (SRC / "runtime/AppWiring.cpp").read_text(errors="replace")
for needle in ("setResponseIndex(responseIndex)", "setExecutionGate(executionGate)",
               "sessionStore->setResponseIndex(responseIndex.get())",
               "ctx.setAiApiUseCase(aiApiUseCase)"):
    if needle not in wiring:
        errors.append(f"runtime session service wiring missing: {needle}")

if errors:
    print("P5-W2 session service injection gate FAIL")
    for error in errors:
        print(f"  {error}")
    raise SystemExit(FAIL)

print("PASS P5-W2: ResponseIndex/ExecutionGate and the AI facade receive injected session services")
