#!/usr/bin/env python3
"""P5-W3 lifecycle increment: context-owned services have no singleton fallback.

Also guards static Controller bindings whose lifetime is shorter than the
context-owned collaborators they borrow.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4
errors = []

# A runtime-owned worker/store may not be recoverable through a Meyers
# singleton in any production path.  Such a fallback creates a second owner
# during a partial migration and makes the stop/deadline path unknowable.
for path in SRC.rglob("*"):
    if path.suffix not in {".h", ".hpp", ".cpp", ".cc"} or "test" in path.parts:
        continue
    relative = path.relative_to(SRC).as_posix()
    text = path.read_text(errors="replace")
    for symbol in (
        "chatSession::getInstance",
        "chaynsThreadReaper::getInstance",
        "BackgroundTaskQueue::instance",
        "SessionDbManager::getInstance",
        "chaynsThreadDbManager::getInstance",
        "ErrorStatsService::getInstance",
        "ErrorStatsDbManager::getInstance",
        "StatusDbManager::getInstance",
        "ErrorStatsConfig::getInstance",
        "ErrorStatsConfig::initFromApp",
        "AccountManager::getInstance",
        "ChannelManager::getInstance",
        "AccountDbManager::getInstance",
        "AccountBackupDbManager::getInstance",
        "ChannelDbManager::getInstance",
        "ConfigDbManager::getInstance",
        "RetoolWorkspaceDbManager::getInstance",
        "RetoolWorkspaceManager::getInstance",
        "RetoolWorkspaceService::getInstance",
    ):
        for match in re.finditer(re.escape(symbol) + r"\s*\(", text):
            line = text.count("\n", 0, match.start()) + 1
            errors.append(f"{relative}:{line}: lifecycle service locator revived: {symbol}")

session_header = (SRC / "sessionManager/core/Session.h").read_text(errors="replace")
reaper_header = (SRC / "infrastructure/provider/chayns/chaynsThreadReaper.h").read_text(errors="replace")
queue_header = (SRC / "infrastructure/executor/BackgroundTaskQueue.h").read_text(errors="replace")
session_db_header = (SRC / "dbManager/session/SessionDbManager.h").read_text(errors="replace")
thread_db_header = (SRC / "dbManager/chaynsThread/chaynsThreadDbManager.h").read_text(errors="replace")
error_stats_db_header = (SRC / "dbManager/metrics/ErrorStatsDbManager.h").read_text(errors="replace")
status_db_header = (SRC / "dbManager/metrics/StatusDbManager.h").read_text(errors="replace")
account_db_header = (SRC / "dbManager/account/accountDbManager.h").read_text(errors="replace")
account_backup_db_header = (SRC / "dbManager/account/accountBackupDbManager.h").read_text(
    errors="replace"
)
channel_db_header = (SRC / "dbManager/channel/channelDbManager.h").read_text(errors="replace")
config_db_header = (SRC / "dbManager/config/ConfigDbManager.h").read_text(errors="replace")
workspace_db_header = (SRC / "dbManager/retoolWorkspace/RetoolWorkspaceDbManager.h").read_text(
    errors="replace"
)
error_stats_service_header = (SRC / "metrics/ErrorStatsService.h").read_text(errors="replace")
error_stats_service_cpp = (SRC / "metrics/ErrorStatsService.cpp").read_text(errors="replace")
error_stats_config_header = (SRC / "metrics/ErrorStatsConfig.h").read_text(errors="replace")
account_manager_header = (SRC / "accountManager/accountManager.h").read_text(errors="replace")
channel_manager_header = (SRC / "channelManager/channelManager.h").read_text(errors="replace")
workspace_manager_header = (SRC / "retoolWorkspace/RetoolWorkspaceManager.h").read_text(
    errors="replace"
)
workspace_service_header = (SRC / "retoolWorkspace/RetoolWorkspaceService.h").read_text(
    errors="replace"
)
ai_controller_header = (SRC / "controllers/AiApiController.h").read_text(errors="replace")
ai_controller_cpp = (SRC / "controllers/AiApiController.cc").read_text(errors="replace")
ai_use_case_header = (SRC / "sessionManager/core/AiApiUseCase.h").read_text(errors="replace")
ai_use_case_cpp = (SRC / "sessionManager/core/AiApiUseCase.cpp").read_text(errors="replace")
context = (SRC / "runtime/AppContext.h").read_text(errors="replace")
wiring = (SRC / "runtime/AppWiring.cpp").read_text(errors="replace")

if re.search(r"static\s+chatSession\s*\*\s*getInstance\s*\(", session_header):
    errors.append("Session.h still exposes chatSession singleton construction")
if re.search(r"static\s+chaynsThreadReaper\s*&\s*getInstance\s*\(", reaper_header):
    errors.append("chaynsThreadReaper.h still exposes singleton construction")
if re.search(r"static\s+BackgroundTaskQueue\s*&\s*instance\s*\(", queue_header):
    errors.append("BackgroundTaskQueue.h still exposes a process singleton")
if re.search(r"static\s+std::shared_ptr<\s*SessionDbManager\s*>\s+getInstance\s*\(", session_db_header):
    errors.append("SessionDbManager.h still exposes singleton construction")
if re.search(r"static\s+std::shared_ptr<\s*chaynsThreadDbManager\s*>\s+getInstance\s*\(", thread_db_header):
    errors.append("chaynsThreadDbManager.h still exposes singleton construction")
if re.search(r"static\s+std::shared_ptr<\s*ErrorStatsDbManager\s*>\s+getInstance\s*\(", error_stats_db_header):
    errors.append("ErrorStatsDbManager.h still exposes singleton construction")
if re.search(r"static\s+std::shared_ptr<\s*StatusDbManager\s*>\s+getInstance\s*\(", status_db_header):
    errors.append("StatusDbManager.h still exposes singleton construction")
for name, header in (
    ("AccountDbManager", account_db_header),
    ("AccountBackupDbManager", account_backup_db_header),
    ("ChannelDbManager", channel_db_header),
):
    if re.search(
        rf"static\s+(?:std::)?shared_ptr\s*<\s*{name}\s*>\s+getInstance\s*\(",
        header,
    ):
        errors.append(f"{name}.h still exposes singleton construction")
    if "bool initialize(std::string* errorMessage = nullptr);" not in header:
        errors.append(f"{name} no longer requires explicit runtime initialization")
    if "drogon::app()" in header:
        errors.append(f"{name}.h revived a construction-time Drogon app lookup")
if re.search(
    r"static\s+(?:std::)?shared_ptr\s*<\s*ConfigDbManager\s*>\s+getInstance\s*\(",
    config_db_header,
):
    errors.append("ConfigDbManager.h still exposes singleton construction")
if "void initialize();" not in config_db_header:
    errors.append("ConfigDbManager no longer requires explicit runtime initialization")
if "drogon::app()" in config_db_header:
    errors.append("ConfigDbManager.h revived a construction-time Drogon app lookup")
if re.search(
    r"static\s+(?:std::)?shared_ptr\s*<\s*RetoolWorkspaceDbManager\s*>\s+getInstance\s*\(",
    workspace_db_header,
):
    errors.append("RetoolWorkspaceDbManager.h still exposes singleton construction")
if "void initialize();" not in workspace_db_header:
    errors.append("RetoolWorkspaceDbManager no longer requires explicit runtime initialization")
if "drogon::app()" in workspace_db_header:
    errors.append("RetoolWorkspaceDbManager.h revived a construction-time Drogon app lookup")
if re.search(r"static\s+ErrorStatsService\s*&\s+getInstance\s*\(", error_stats_service_header):
    errors.append("ErrorStatsService.h still exposes singleton construction")
if "explicit ErrorStatsService(std::shared_ptr<IErrorStatsSink> sink)" not in error_stats_service_header:
    errors.append("ErrorStatsService no longer requires an injected IErrorStatsSink")
if "ErrorStatsDbManager.h" in error_stats_service_cpp:
    errors.append("ErrorStatsService.cpp still includes concrete ErrorStatsDbManager fallback")
if re.search(r"static\s+ErrorStatsConfig\s*&\s+getInstance\s*\(", error_stats_config_header):
    errors.append("ErrorStatsConfig.h still exposes a process singleton")
if re.search(r"static\s+AccountManager\s*&\s+getInstance\s*\(", account_manager_header):
    errors.append("AccountManager.h still exposes a process singleton")
if "public IAccountSelector" not in account_manager_header:
    errors.append("AccountManager no longer exposes the injected IAccountSelector port")
if re.search(r"static\s+ChannelManager\s*&\s+getInstance\s*\(", channel_manager_header):
    errors.append("ChannelManager.h still exposes a process singleton")
if re.search(r"static\s+RetoolWorkspaceManager\s*&\s+getInstance\s*\(", workspace_manager_header):
    errors.append("RetoolWorkspaceManager.h still exposes a process singleton")
if "explicit RetoolWorkspaceManager(std::shared_ptr<IRetoolWorkspaceStore> store)" not in workspace_manager_header:
    errors.append("RetoolWorkspaceManager no longer requires constructor store injection")
if "RetoolWorkspaceManager(const RetoolWorkspaceManager&) = delete;" not in workspace_manager_header:
    errors.append("RetoolWorkspaceManager can be copied around its construction dependency")
if "setStore(" in workspace_manager_header:
    errors.append("RetoolWorkspaceManager still exposes mutable store injection")
if re.search(r"static\s+RetoolWorkspaceService\s*&\s+getInstance\s*\(", workspace_service_header):
    errors.append("RetoolWorkspaceService.h still exposes a process singleton")
if "explicit RetoolWorkspaceService(std::shared_ptr<IRetoolWorkspaceStore> workspaceStore)" not in workspace_service_header:
    errors.append("RetoolWorkspaceService no longer requires constructor store injection")
if "RetoolWorkspaceService(const RetoolWorkspaceService&) = delete;" not in workspace_service_header:
    errors.append("RetoolWorkspaceService can be copied around its construction dependency")
if "setWorkspaceStore(" in workspace_service_header:
    errors.append("RetoolWorkspaceService still exposes mutable store injection")
if not re.search(r"explicit\s+chaynsThreadReaper\s*\(\s*std::shared_ptr<\s*chaynsThreadDbManager\s*>", reaper_header):
    errors.append("ThreadReaper no longer requires an injected thread ledger")
# P5-W3 completion: Drogon receives one raw controller-facing facade, never a
# bag of provider/session/index/gate/executor pointers.  The facade itself is
# the sole legacy-generation composition seam and snapshots dependencies at
# queue admission.
for needle in (
    "static void setUseCase(aiapi::IAiApiUseCase* useCase)",
    "static aiapi::IAiApiUseCase* useCase_",
):
    if needle not in ai_controller_header:
        errors.append(f"AiApiController use-case binding missing: {needle}")
for forbidden in (
    "setProviderRegistry", "setSessionServices", "setBackgroundExecutor",
    "setChannelCatalog", "clearServices", "GenerationService", "RequestAdapters",
    "IProviderRegistry", "IResponseIndex", "IExecutionGate", "IBackgroundExecutor",
    "IChannelCatalog", "chatSession",
):
    if forbidden in ai_controller_header or forbidden in ai_controller_cpp:
        errors.append(f"AiApiController still owns a legacy collaborator: {forbidden}")

for needle in (
    "class AiApiUseCase final : public aiapi::IAiApiUseCase",
    "IProviderRegistry* providers_",
    "chatSession* sessions_",
    "IResponseIndex* responses_",
    "session::IExecutionGate* executionGate_",
    "IChannelCatalog* channels_",
    "IBackgroundExecutor* executor_",
):
    if needle not in ai_use_case_header:
        errors.append(f"AiApiUseCase collaborator declaration missing: {needle}")
for needle in (
    "RequestAdapters::buildGenerationRequestFrom",
    "auto* const providers = providers_",
    "auto* const sessions = sessions_",
    "auto* const responses = responses_",
    "auto* const executionGate = executionGate_",
    "auto* const channels = channels_",
    "auto* const executor = executor_",
    "executor->submit(",
    "GenerationService generation(",
    "responses->storeResponse(",
    "AiApiUseCase::modelCatalog",
    "AiApiUseCase::getResponse",
    "AiApiUseCase::deleteResponse",
):
    if needle not in ai_use_case_cpp:
        errors.append(f"AiApiUseCase orchestration/snapshot missing: {needle}")

for needle in (
    "std::shared_ptr<chatSession> sessionStore_",
    "std::shared_ptr<chaynsThreadReaper> threadReaper_",
    "std::shared_ptr<BackgroundTaskQueue> backgroundTaskQueue_",
    "std::shared_ptr<SessionDbManager> sessionPersistence_",
    "std::shared_ptr<chaynsThreadDbManager> threadLedger_",
    "std::shared_ptr<metrics::ErrorStatsDbManager> errorStatsStore_",
    "std::shared_ptr<metrics::StatusDbManager> statusMetricsStore_",
    "std::shared_ptr<metrics::ErrorStatsService> errorStatsService_",
    "std::shared_ptr<metrics::IMetricsUseCase> metricsUseCase_",
    "std::shared_ptr<ChannelDbManager> channelStore_",
    "std::shared_ptr<ChannelManager> channelManager_",
    "std::shared_ptr<ConfigDbManager> configStore_",
    "std::shared_ptr<AccountDbManager> accountStore_",
    "std::shared_ptr<AccountBackupDbManager> accountBackupStore_",
    "std::shared_ptr<RetoolWorkspaceDbManager> retoolWorkspaceStore_",
    "std::shared_ptr<RetoolWorkspaceManager> retoolWorkspaceManager_",
    "std::shared_ptr<workspace::IRetoolWorkspaceProvisioner> retoolWorkspaceProvisioner_",
    "std::shared_ptr<workspace::IRetoolWorkspaceUseCase> retoolWorkspaceUseCase_",
    "std::shared_ptr<workspace::IRetoolWorkspaceAdminUseCase> retoolWorkspaceAdminUseCase_",
    "std::shared_ptr<AccountManager> accountManager_",
    "std::shared_ptr<IAccountAdminUseCase> accountAdminUseCase_",
    "std::shared_ptr<IChannelAdminUseCase> channelAdminUseCase_",
    "std::shared_ptr<IHealthUseCase> healthUseCase_",
    "std::shared_ptr<aiapi::IAiApiUseCase> aiApiUseCase_",
    "setAiApiUseCase(std::shared_ptr<aiapi::IAiApiUseCase>",
    "setBackgroundTaskQueue(std::shared_ptr<BackgroundTaskQueue>",
    "setSessionPersistence(std::shared_ptr<SessionDbManager>",
    "setThreadLedger(std::shared_ptr<chaynsThreadDbManager>",
    "setErrorStatsStore(std::shared_ptr<metrics::ErrorStatsDbManager>",
    "setStatusMetricsStore(std::shared_ptr<metrics::StatusDbManager>",
    "setErrorStatsService(std::shared_ptr<metrics::ErrorStatsService>",
    "setMetricsUseCase(std::shared_ptr<metrics::IMetricsUseCase>",
    "setChannelStore(std::shared_ptr<ChannelDbManager>",
    "setChannelManager(std::shared_ptr<ChannelManager>",
    "setConfigStore(std::shared_ptr<ConfigDbManager>",
    "setAccountStore(std::shared_ptr<AccountDbManager>",
    "setAccountBackupStore(std::shared_ptr<AccountBackupDbManager>",
    "setRetoolWorkspaceStore(std::shared_ptr<RetoolWorkspaceDbManager>",
    "setRetoolWorkspaceManager(std::shared_ptr<RetoolWorkspaceManager>",
    "setRetoolWorkspaceProvisioner(\n        std::shared_ptr<workspace::IRetoolWorkspaceProvisioner>",
    "setRetoolWorkspaceUseCase(std::shared_ptr<workspace::IRetoolWorkspaceUseCase>",
    "setRetoolWorkspaceAdminUseCase(\n        std::shared_ptr<workspace::IRetoolWorkspaceAdminUseCase>",
    "setAccountManager(std::shared_ptr<AccountManager>",
    "setAccountAdminUseCase(std::shared_ptr<IAccountAdminUseCase>",
    "setChannelAdminUseCase(std::shared_ptr<IChannelAdminUseCase>",
    "setHealthUseCase(std::shared_ptr<IHealthUseCase>",
    "setThreadReaper(std::shared_ptr<chaynsThreadReaper>",
):
    if needle not in context:
        errors.append(f"AppContext lifecycle ownership missing: {needle}")

for needle in (
    "ctx.setSessionStore(std::make_shared<chatSession>())",
    "ctx.setBackgroundTaskQueue(std::make_shared<BackgroundTaskQueue>())",
    "std::make_shared<SessionDbManager>(executor)",
    "ctx.setSessionPersistence(std::move(sessionDb))",
    "std::make_shared<chaynsThreadDbManager>(executor)",
    "ctx.setThreadLedger(std::move(threadDb))",
    "ctx.setErrorStatsStore(std::make_shared<metrics::ErrorStatsDbManager>())",
    "ctx.setStatusMetricsStore(std::make_shared<metrics::StatusDbManager>())",
    "std::make_shared<metrics::ErrorStatsService>(errorStore)",
    "ctx.setErrorStatsService(errorStats)",
    "std::make_shared<metrics::MetricsUseCase>(",
    "ctx.setMetricsUseCase(metricsUseCase)",
    "MetricsController::setUseCase(metricsUseCase.get())",
    "MetricsController::setUseCase(nullptr)",
    "[errorStats](std::chrono::steady_clock::time_point deadline)",
    "errorStats->shutdown(deadline)",
    "std::make_shared<AccountManager>()",
    "ctx.setAccountManager(accounts)",
    "auto channelStore = std::make_shared<ChannelDbManager>()",
    "channelStore->initialize(&storeError)",
    "ctx.setChannelStore(channelStore)",
    "std::make_shared<ChannelManager>()",
    "ctx.setChannelManager(channels)",
    "channels->setStore(channelStore)",
    "channels->init()",
    "auto configStore = std::make_shared<ConfigDbManager>()",
    "configStore->initialize()",
    "ctx.setConfigStore(configStore)",
    "accounts->setConfigStore(configStore)",
    "auto accountStore = std::make_shared<AccountDbManager>()",
    "accountStore->initialize(&storeError)",
    "ctx.setAccountStore(accountStore)",
    "auto accountBackupStore = std::make_shared<AccountBackupDbManager>()",
    "accountBackupStore->initialize(&storeError)",
    "ctx.setAccountBackupStore(accountBackupStore)",
    "accounts->setStore(accountStore)",
    "accounts->setChannelStore(channelStore)",
    "std::make_shared<ChannelAdminUseCase>(",
    "ctx.setChannelAdminUseCase(channelAdmin)",
    "ChannelController::setUseCase(channelAdmin.get())",
    "ChannelController::setUseCase(nullptr)",
    "auto workspaceStore = std::make_shared<RetoolWorkspaceDbManager>()",
    "workspaceStore->initialize()",
    "ctx.setRetoolWorkspaceStore(workspaceStore)",
    "std::make_shared<RetoolWorkspaceManager>(workspaceStore)",
    "ctx.setRetoolWorkspaceManager(workspaceManager)",
    "workspaceManager->init()",
    "std::make_shared<RetoolWorkspaceService>(workspaceStore)",
    "ctx.setRetoolWorkspaceProvisioner(workspaceProvisioner)",
    "std::make_shared<workspace::RetoolWorkspaceUseCase>(",
    "ctx.setRetoolWorkspaceUseCase(workspaceUseCase)",
    "std::make_shared<workspace::RetoolWorkspaceAdminUseCase>(",
    "ctx.setRetoolWorkspaceAdminUseCase(workspaceAdmin)",
    "RetoolWorkspaceController::setUseCase(workspaceAdmin.get())",
    "RetoolWorkspaceController::setUseCase(nullptr)",
    "accounts->setRetoolWorkspaceServices(nullptr, nullptr)",
    "addAccountWorkersOwner(ctx, accounts)",
    "[accounts](std::chrono::steady_clock::time_point deadline)",
    "accounts->stopBackgroundThreads(deadline)",
    "provider::makeProductionProvider<chaynsapi>",
    "std::make_shared<HealthUseCase>(",
    "ctx.setHealthUseCase(healthUseCase)",
    "HealthController::setUseCase(healthUseCase.get())",
    "HealthController::setUseCase(nullptr)",
    "std::make_shared<AiApiUseCase>(",
    "ctx.setAiApiUseCase(aiApiUseCase)",
    "AiApiController::setUseCase(aiApiUseCase.get())",
    "ctx.addOwner(\"AI API controller binding\"",
    "AiApiController::setUseCase(nullptr)",
    "std::make_shared<chaynsThreadReaper>(threadDb)",
    "ctx.setThreadReaper(reaper)",
    "[queue](std::chrono::steady_clock::time_point deadline)",
    "queue->shutdown(deadline)",
    "[reaper](std::chrono::steady_clock::time_point deadline)",
    "reaper->stop(deadline)",
):
    if needle not in wiring:
        errors.append(f"runtime lifecycle wiring missing: {needle}")

channel_store_construction = "auto channelStore = std::make_shared<ChannelDbManager>()"
channel_store_initialize = "channelStore->initialize(&storeError)"
channel_store_publish = "ctx.setChannelStore(channelStore)"
channel_manager_init = "channels->init()"
if all(needle in wiring for needle in (
    channel_store_construction,
    channel_store_initialize,
    channel_store_publish,
    channel_manager_init,
)):
    if not (
        wiring.index(channel_store_construction)
        < wiring.index(channel_store_initialize)
        < wiring.index(channel_store_publish)
        < wiring.index(channel_manager_init)
    ):
        errors.append(
            "ChannelDbManager must be initialized and AppContext-published before "
            "ChannelManager::init()"
        )

config_store_construction = "auto configStore = std::make_shared<ConfigDbManager>()"
config_store_initialize = "configStore->initialize()"
config_store_publish = "ctx.setConfigStore(configStore)"
account_manager_init = "accounts->init()"
if all(needle in wiring for needle in (
    config_store_construction,
    config_store_initialize,
    config_store_publish,
    account_manager_init,
)):
    if not (
        wiring.index(config_store_construction)
        < wiring.index(config_store_initialize)
        < wiring.index(config_store_publish)
        < wiring.index(account_manager_init)
    ):
        errors.append(
            "ConfigDbManager must be initialized and AppContext-published before "
            "AccountManager::init()"
        )

account_store_construction = "auto accountStore = std::make_shared<AccountDbManager>()"
account_store_initialize = "accountStore->initialize(&storeError)"
account_store_publish = "ctx.setAccountStore(accountStore)"
if all(needle in wiring for needle in (
    account_store_construction,
    account_store_initialize,
    account_store_publish,
    account_manager_init,
)):
    if not (
        wiring.index(account_store_construction)
        < wiring.index(account_store_initialize)
        < wiring.index(account_store_publish)
        < wiring.index(account_manager_init)
    ):
        errors.append(
            "AccountDbManager must be initialized and AppContext-published before "
            "AccountManager::init()"
        )

health_use_case_construction = "std::make_shared<HealthUseCase>("
health_use_case_publish = "ctx.setHealthUseCase(healthUseCase)"
if all(needle in wiring for needle in (
    account_store_publish,
    health_use_case_construction,
    health_use_case_publish,
)):
    if not (
        wiring.index(account_store_publish)
        < wiring.index(health_use_case_construction)
        < wiring.index(health_use_case_publish)
    ):
        errors.append(
            "HealthUseCase must be constructed from the AppContext-published "
            "AccountDbManager before it is published to HealthController"
        )

ai_use_case_construction = "std::make_shared<AiApiUseCase>("
ai_use_case_publish = "ctx.setAiApiUseCase(aiApiUseCase)"
ai_controller_bind = "AiApiController::setUseCase(aiApiUseCase.get())"
ai_binding_owner = "ctx.addOwner(\"AI API controller binding\""
if all(needle in wiring for needle in (
    ai_use_case_construction, ai_use_case_publish, ai_controller_bind, ai_binding_owner,
)):
    if not (
        wiring.index(ai_use_case_construction)
        < wiring.index(ai_use_case_publish)
        < wiring.index(ai_controller_bind)
        < wiring.index(ai_binding_owner)
    ):
        errors.append(
            "AiApiUseCase must be context-published before controller binding and rollback owner"
        )

backup_store_construction = "auto accountBackupStore = std::make_shared<AccountBackupDbManager>()"
backup_store_initialize = "accountBackupStore->initialize(&storeError)"
backup_store_publish = "ctx.setAccountBackupStore(accountBackupStore)"
account_admin_construction = "auto accountAdmin = std::make_shared<AccountAdminUseCase>("
if all(needle in wiring for needle in (
    backup_store_construction,
    backup_store_initialize,
    backup_store_publish,
    account_admin_construction,
)):
    if not (
        wiring.index(backup_store_construction)
        < wiring.index(backup_store_initialize)
        < wiring.index(backup_store_publish)
        < wiring.index(account_admin_construction)
    ):
        errors.append(
            "AccountBackupDbManager must be initialized and AppContext-published before "
            "AccountAdminUseCase construction"
        )

workspace_store_construction = "auto workspaceStore = std::make_shared<RetoolWorkspaceDbManager>()"
workspace_store_initialize = "workspaceStore->initialize()"
workspace_store_publish = "ctx.setRetoolWorkspaceStore(workspaceStore)"
workspace_manager_init = "workspaceManager->init()"
if all(needle in wiring for needle in (
    workspace_store_construction,
    workspace_store_initialize,
    workspace_store_publish,
    workspace_manager_init,
)):
    if not (
        wiring.index(workspace_store_construction)
        < wiring.index(workspace_store_initialize)
        < wiring.index(workspace_store_publish)
        < wiring.index(workspace_manager_init)
    ):
        errors.append(
            "RetoolWorkspaceDbManager must be initialized and AppContext-published before "
            "RetoolWorkspaceManager::init()"
        )

for relative in (
    "dbManager/session/SessionDbManager.cpp",
    "dbManager/chaynsThread/chaynsThreadDbManager.cpp",
):
    text = (SRC / relative).read_text(errors="replace")
    if "BackgroundTaskQueue.h" in text:
        errors.append(f"{relative}: concrete DB manager still includes BackgroundTaskQueue")
    if "executor_->submit(" not in text:
        errors.append(f"{relative}: async write-through no longer uses injected executor")

provider = (SRC / "infrastructure/provider/chayns/chaynsapi.cpp").read_text(errors="replace")
if "m_threadLedger->" not in provider:
    errors.append("chaynsapi no longer uses its injected thread ledger")

if re.search(r"static\s+workspace::RetoolWorkspaceUseCase\b", wiring):
    errors.append("AppWiring revived a function-static RetoolWorkspaceUseCase")

if errors:
    print("P5-W3 lifecycle service injection gate FAIL")
    for error in errors:
        print(f"  {error}")
    raise SystemExit(FAIL)

print("PASS P5-W3: runtime, concrete stores, and the AI controller facade are AppContext-owned")
