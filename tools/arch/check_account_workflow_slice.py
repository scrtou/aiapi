#!/usr/bin/env python3
"""P7-W2 ratchet for AccountManager workflow ownership."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
from typing import Mapping

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4

CMAKE = SRC / "CMakeLists.txt"
CORE = SRC / "accountManager/accountManager.cpp"
SELECTOR = SRC / "accountManager/AccountSelector.cpp"
POLICY = SRC / "accountManager/AccountSelectionPolicy.cpp"
STATE_HEADER = SRC / "accountManager/AccountRegistrationStateMachine.h"
STATE = SRC / "accountManager/AccountRegistrationStateMachine.cpp"
TOKEN = SRC / "accountManager/AccountTokenWorkflow.cpp"
REGISTRATION = SRC / "accountManager/AccountRegistrationWorkflow.cpp"
HEALTH = SRC / "accountManager/AccountHealthWorkflow.cpp"
WORKERS = SRC / "accountManager/AccountWorkers.cpp"
SUPPORT = SRC / "accountManager/AccountWorkflowSupport.cpp"
TEST = SRC / "test/test_account_workflow_stages.cpp"
LIFECYCLE_TEST = SRC / "test/test_account_lifecycle_fixture.cpp"
SHUTDOWN_TEST = SRC / "test/test_shutdown_workers.cpp"

APPLICATION_SOURCES = (
    "accountManager/accountManager.cpp",
    "accountManager/AccountWorkflowSupport.cpp",
    "accountManager/AccountSelectionPolicy.cpp",
    "accountManager/AccountRegistrationStateMachine.cpp",
    "accountManager/AccountSelector.cpp",
    "accountManager/AccountTokenWorkflow.cpp",
    "accountManager/AccountRegistrationWorkflow.cpp",
    "accountManager/AccountHealthWorkflow.cpp",
    "accountManager/AccountWorkers.cpp",
)


def uncommented(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def cmake_source_list(cmake: str, name: str) -> str:
    match = re.search(r"set\(\s*" + re.escape(name) + r"\s*(.*?)\n\)", cmake, flags=re.S)
    return "" if match is None else match.group(1)


def function_body(text: str, signature: str) -> str:
    start = text.find(signature)
    if start < 0:
        return ""
    open_brace = text.find("{", start)
    if open_brace < 0:
        return ""
    depth = 0
    for index in range(open_brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    return ""


def validate(overrides: Mapping[Path, str] | None = None) -> list[str]:
    overrides = overrides or {}
    errors: list[str] = []

    def read(path: Path) -> str:
        if path in overrides:
            return overrides[path]
        if not path.is_file():
            errors.append(f"missing required P7-W2 file: {path.relative_to(ROOT)}")
            return ""
        return path.read_text(errors="replace")

    cmake = read(CMAKE)
    core = uncommented(read(CORE))
    selector = uncommented(read(SELECTOR))
    policy = uncommented(read(POLICY))
    state_header = uncommented(read(STATE_HEADER))
    state = uncommented(read(STATE))
    token = uncommented(read(TOKEN))
    registration = uncommented(read(REGISTRATION))
    health = uncommented(read(HEALTH))
    workers = uncommented(read(WORKERS))
    support = uncommented(read(SUPPORT))
    test = read(TEST)
    lifecycle_test = read(LIFECYCLE_TEST)
    shutdown_test = read(SHUTDOWN_TEST)

    application = cmake_source_list(cmake, "AIAPI_APPLICATION_SOURCES")
    legacy = cmake_source_list(cmake, "AIAPI_LEGACY_SOURCES")
    for source in APPLICATION_SOURCES:
        if source not in cmake:
            errors.append(f"src/CMakeLists.txt must register P7-W2 source: {source}")
        if source not in application:
            errors.append(f"P7-W2 source must be owned by aiapi_application: {source}")
        if source in legacy:
            errors.append(f"P7-W2 source must not return to aiapi_legacy: {source}")

    # The remaining AccountManager file is construction/configuration only.
    # A future monolith cannot hide behind the original filename again.
    for stale in (
        "AccountManager::getEligibleAccount",
        "AccountManager::getAccountByUserName",
        "AccountManager::checkToken",
        "AccountManager::autoRegisterAccount",
        "AccountManager::checkChannelAccountCount",
        "AccountManager::stopBackgroundThreads",
    ):
        if stale in core:
            errors.append(f"AccountManager core facade revived workflow implementation: {stale}")
    for required in (
        "AccountManager::setStore",
        "AccountManager::requireStore",
        "AccountManager::loadAccountAutomationSettings",
        "AccountManager::init",
        "std::make_unique<account::AccountRegistrationStateMachine>",
    ):
        if required not in core:
            errors.append(f"AccountManager core ownership/wiring missing: {required}")

    for required in (
        "AccountManager::getEligibleAccount",
        "AccountManager::getAccountByUserName",
        "AccountManager::setStatusTokenStatus",
        "rebuildPoolLocked(apiName)",
        "account::selection::matchesRequirement",
    ):
        if required not in selector:
            errors.append(f"selector/rotation stage missing: {required}")
    for required in (
        "bool isPoolMember",
        "bool matchesRequirement",
        "workspaceUacId <= 0",
        "AccountRequirement::FreeOnly",
        "AccountRequirement::ProOnly",
    ):
        if required not in policy:
            errors.append(f"pure selector policy missing: {required}")

    for required in (
        "class AccountRegistrationStateMachine",
        "int begin(const std::string& apiName)",
        "void rollback(int waitingId)",
        "bool activate(int waitingId, const Accountinfo_st& account)",
    ):
        if required not in state_header:
            errors.append(f"registration state contract missing: {required}")
    rollback = function_body(state, "void AccountRegistrationStateMachine::rollback")
    wait_at = rollback.find("AccountStatus::WAITING")
    delete_at = rollback.find("deleteWaitingAccount")
    if not rollback or wait_at < 0 or delete_at < 0 or wait_at > delete_at:
        errors.append("registration rollback must restore waiting before deleting reservation")
    for required in (
        "AccountManager::autoRegisterAccount",
        "registrationStateMachine_->begin(apiName)",
        "RegistrationScope scope",
        "rollbackWaitingAccount(waitingId)",
        "registrationStateMachine_->activate(waitingId, account)",
    ):
        if required not in registration:
            errors.append(f"registration workflow stage missing: {required}")

    for required in (
        "AccountManager::checkChaynsToken",
        "AccountManager::getChaynsToken",
        "AccountManager::checkToken",
        "AccountManager::updateToken",
        "AccountManager::waitUpdateAccountToken",
        "AccountManager::isServerReachable",
    ):
        if required not in token:
            errors.append(f"token refresh stage missing: {required}")
    for required in (
        "AccountManager::checkChannelAccountCount",
        "AccountManager::cleanExpiredAccounts",
        "AccountManager::deleteUpstreamAccount",
        "AccountManager::updateAllAccountTypes",
    ):
        if required not in health:
            errors.append(f"health workflow stage missing: {required}")
    for required in (
        "AccountManager::stopBackgroundThreads",
        "AccountManager::checkUpdateTokenthread",
        "AccountManager::waitUpdateAccountTokenThread",
        "AccountManager::checkAccountTypeThread",
        "platform::joinUntil",
    ):
        if required not in workers:
            errors.append(f"worker ownership stage missing: {required}")
    for required in (
        "std::string loginServiceUrl",
        "std::string registrationServiceUrl",
        "bool splitUrl",
        "bool isSuccessEnvelope",
    ):
        if required not in support:
            errors.append(f"shared workflow support missing: {required}")

    for name in (
        "AccountWorkflow_StateMachineRollbackRestoresWaitingThenDeletes",
        "AccountWorkflow_StateMachineTransitionFailureRollsBackReservation",
        "AccountWorkflow_SelectorAppliesBindingRequirementAndRotationFilter",
        "AccountWorkflow_SupportParsesWorkflowEndpointsAndEnvelopes",
    ):
        if name not in test:
            errors.append(f"P7-W2 stage regression missing: {name}")
    for name in (
        "AccountLifecycle_AutoRegisterHttpFailureRollsBackWaitingRow",
        "AccountLifecycle_AutoRegisterSuccessActivatesAndLoadsAccount",
        "AccountLifecycle_CheckTokenUsesFakeHttpAndInvalidatesPool",
    ):
        if name not in lifecycle_test:
            errors.append(f"P7-W2 production workflow coverage missing: {name}")
    for name in (
        "ShutdownWorkers_LongWaitsAreInterruptibleAndStopsAreIdempotent",
        "ShutdownWorkers_ServerReachabilityRetryLoopStopsImmediately",
    ):
        if name not in shutdown_test:
            errors.append(f"P7-W2 worker regression missing: {name}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="P7-W2 Account workflow slice ratchet")
    parser.add_argument("--selftest", action="store_true",
                        help="prove rollback-order mutation is rejected in memory")
    args = parser.parse_args()

    errors = validate()
    if errors:
        print("P7-W2 Account workflow slice gate FAIL")
        for error in errors:
            print(f"  {error}")
        return FAIL

    if args.selftest:
        original = STATE.read_text(errors="replace")
        mutation = original.replace("AccountStatus::WAITING", "AccountStatus::ACTIVE")
        if mutation == original:
            print("P7-W2 Account workflow slice gate selftest FAIL")
            print("  probe could not mutate rollback transition")
            return FAIL
        mutation_errors = validate({STATE: mutation})
        if not mutation_errors:
            print("P7-W2 Account workflow slice gate selftest FAIL")
            print("  rollback-order mutation unexpectedly passed")
            return FAIL
        print("PASS P7-W2 Account workflow slice gate selftest: rollback mutation was rejected")

    print("PASS P7-W2: Account selection, state transitions, workflows, and workers stay split")
    return 0


if __name__ == "__main__":
    sys.exit(main())
