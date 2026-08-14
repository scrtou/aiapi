#!/usr/bin/env python3
"""P5-W3: Account application paths must not locate concrete services."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4
errors = []

for relative in (
    "accountManager/accountManager.cpp",
    "application/account/AccountAdminUseCase.cpp",
    "apipoint/chaynsapi/chaynsapi.cpp",
):
    text = (SRC / relative).read_text(errors="replace")
    for match in re.finditer(r"::\s*(?:getInstance|instance)\s*\(", text):
        line = text.count("\n", 0, match.start()) + 1
        errors.append(f"{relative}:{line}: account path locates a global service")

wiring = (SRC / "runtime/AppWiring.cpp").read_text(errors="replace")
for needle in (
    "ctx.setAccountManager(accounts)",
    "ctx.setAccountStore(accountStore)",
    "ctx.setAccountBackupStore(accountBackupStore)",
    "ctx.setChannelStore(channelStore)",
    "ctx.setChannelManager(channels)",
    "ctx.setConfigStore(configStore)",
    "accounts->setStore(accountStore)",
    "accounts->setChannelStore(channelStore)",
    "accounts->setConfigStore(configStore)",
    "accounts->setRetoolWorkspaceServices(",
    "ctx.setAccountAdminUseCase(accountAdmin)",
    "AccountController::setUseCase(accountAdmin.get())",
    "accountStore.get(),",
    "accountBackupStore.get(), channels.get(), executor",
):
    if needle not in wiring:
        errors.append(f"runtime account wiring missing: {needle}")

account_header = (SRC / "accountManager/accountManager.h").read_text(errors="replace")
if "public IAccountSelector" not in account_header:
    errors.append("AccountManager no longer implements the narrow IAccountSelector port")
if re.search(r"static\s+AccountManager\s*&\s*getInstance\s*\(", account_header):
    errors.append("AccountManager.h still exposes a process singleton")

chayns_header = (SRC / "apipoint/chaynsapi/chaynsapi.h").read_text(errors="replace")
if "IAccountSelector& accountSelector" not in chayns_header:
    errors.append("chaynsapi no longer requires an injected IAccountSelector")
if "accountManager/accountManager.h" in chayns_header:
    errors.append("chaynsapi still includes concrete AccountManager")
if re.search(r"static\s+AccountAdminUseCase\b", wiring):
    errors.append("AppWiring still keeps AccountAdminUseCase as a function-static object")

if errors:
    print("P5-W3 account service injection gate FAIL")
    for error in errors:
        print(f"  {error}")
    raise SystemExit(FAIL)

print("PASS P5-W3: AccountManager/application account paths use injected services")
