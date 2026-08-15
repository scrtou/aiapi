#!/usr/bin/env python3
"""P5-W3: managed account/provider paths must use composition-root injection."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4
errors = []

for relative in (
    "managedAccount/backends/ClassicProviderAccountBackend.cpp",
    "managedAccount/backends/RetoolWorkspaceBackend.cpp",
    "managedAccount/service/ManagedAccountService.h",
    "managedAccount/service/ManagedAccountService.cpp",
    "infrastructure/provider/retool/retoolapi.cpp",
):
    text = (SRC / relative).read_text(errors="replace")
    for match in re.finditer(r"::\s*(?:getInstance|instance)\s*\(", text):
        line = text.count("\n", 0, match.start()) + 1
        errors.append(f"{relative}:{line}: managed account path locates a global service")

service = (SRC / "managedAccount/service/ManagedAccountService.h").read_text(
    errors="replace"
)
if "static ManagedAccountService" in service:
    errors.append("ManagedAccountService must not expose a singleton accessor")
if "std::shared_ptr<IManagedAccountBackend>" not in service:
    errors.append("ManagedAccountService must own injected backend interfaces")

provider_header = (SRC / "infrastructure/provider/retool/retoolapi.h").read_text(errors="replace")
if re.search(r"\bretoolapi\s*\(\s*\)\s*;", provider_header):
    errors.append("retoolapi must not expose a dependency-free constructor")

wiring = (SRC / "runtime/AppWiring.cpp").read_text(errors="replace")
for needle in (
    "std::make_shared<ClassicProviderAccountBackend>(",
    "std::make_shared<RetoolWorkspaceBackend>(",
    "std::make_shared<ManagedAccountService>(",
    "ctx.setManagedAccountService(managedAccounts)",
    "ctx.setRetoolWorkspaceProvisioner(workspaceProvisioner)",
    "ctx.setRetoolWorkspaceUseCase(workspaceUseCase)",
    "*workspaceUseCase,",
    "*channels)",
    "*managedAccounts,",
):
    if needle not in wiring:
        errors.append(f"runtime managed account wiring missing: {needle}")

if errors:
    print("P5-W3 managed account service injection gate FAIL")
    for error in errors:
        print(f"  {error}")
    raise SystemExit(FAIL)

print("PASS P5-W3: managed account backends/service/provider use injected ports")
