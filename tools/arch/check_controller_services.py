#!/usr/bin/env python3
"""P5-W3 ratchet for controller-facing use-case injection.

Drogon owns Controller construction, so AppWiring may publish one non-owning
static use-case pointer per Controller.  A Controller must not receive a bag
of catalogs, stores, gates, queues, or legacy generation services.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4
errors = []

clean_controllers = (
    "transport/controllers/ChannelController.cc",
    "transport/controllers/HealthController.cc",
    "transport/controllers/MetricsController.cc",
    "transport/controllers/RetoolWorkspaceController.cc",
    "transport/controllers/AccountController.cc",
    "transport/controllers/AiApiController.cc",
)

# P5-W3 removed Controller service locators.  Keep a directory scan so a newly
# introduced Controller cannot bypass the inventory.
controller_dir = SRC / "controllers"
scanned_controllers = tuple(
    path.relative_to(SRC).as_posix()
    for path in sorted(controller_dir.glob("*.cc"))
)
locator = re.compile(r"::\s*(?:getInstance|instance)\s*\(")
for relative in sorted(set(clean_controllers) | set(scanned_controllers)):
    path = SRC / relative
    text = path.read_text(errors="replace")
    for match in locator.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        errors.append(f"{relative}:{line}: controller locates a global service")

# Every business Controller is closed over exactly one controller-facing port.
management = {
    "transport/controllers/AccountController.h": "domain/port/IAccountAdminUseCase.h",
    "transport/controllers/ChannelController.h": "domain/port/IChannelAdminUseCase.h",
    "transport/controllers/HealthController.h": "domain/port/IHealthUseCase.h",
    "transport/controllers/MetricsController.h": "domain/port/IMetricsUseCase.h",
    "transport/controllers/RetoolWorkspaceController.h": "domain/port/IRetoolWorkspaceAdminUseCase.h",
    "transport/controllers/AiApiController.h": "domain/port/IAiApiUseCase.h",
}
for relative, expected_port in management.items():
    text = (SRC / relative).read_text(errors="replace")
    if expected_port not in text:
        errors.append(f"{relative}: missing controller-facing use-case port {expected_port}")

# Explicitly ban the pre-facade collaborators from Controller headers.  Codec,
# sink and HTTP-stream includes remain allowed because they are wire adaptation.
for relative, forbidden in {
    "transport/controllers/ChannelController.h": (
        "IAccountCatalog.h", "IChannelCatalog.h", "IBackgroundExecutor.h"),
    "transport/controllers/HealthController.h": (
        "IAccountStore.h", "IAccountCatalog.h", "IProviderRegistry.h"),
    "transport/controllers/MetricsController.h": ("IMetricsQuery.h",),
    "transport/controllers/RetoolWorkspaceController.h": ("IRetoolWorkspaceUseCase.h",),
    "transport/controllers/AiApiController.h": (
        "IProviderRegistry.h", "IResponseIndex.h", "IExecutionGate.h",
        "IBackgroundExecutor.h", "IChannelCatalog.h", "GenerationService.h",
        "RequestAdapters.h", "Session.h"),
}.items():
    text = (SRC / relative).read_text(errors="replace")
    for header in forbidden:
        if header in text:
            errors.append(f"{relative}: direct collaborator include revived: {header}")

ai_header = (SRC / "transport/controllers/AiApiController.h").read_text(errors="replace")
ai_cpp = (SRC / "transport/controllers/AiApiController.cc").read_text(errors="replace")
for needle in (
    "static void setUseCase(aiapi::IAiApiUseCase* useCase)",
    "static aiapi::IAiApiUseCase* useCase_",
    "useCase->submitGeneration(",
    "useCase->modelCatalog(",
    "useCase->getResponse(",
    "useCase->deleteResponse(",
):
    text = ai_header if needle.startswith("static") else ai_cpp
    if needle not in text:
        errors.append(f"AiApiController closure missing: {needle}")

for forbidden in (
    "GenerationService.h", "RequestAdapters.h", "ResponseIndex.h",
    "SessionExecutionGate.h", "IProviderRegistry", "IResponseIndex",
    "IExecutionGate", "IBackgroundExecutor", "IChannelCatalog", "chatSession",
    "setProviderRegistry", "setSessionServices", "setBackgroundExecutor",
    "setChannelCatalog", "clearServices",
):
    if forbidden in ai_cpp or forbidden in ai_header:
        errors.append(f"AiApiController revived direct legacy collaborator: {forbidden}")

# Persistence must remain inside the facade, not sneak back into the transport
# through a direct response-index call.
for forbidden in ("tryGetResponse(", "storeResponse(", "->erase("):
    if forbidden in ai_cpp:
        errors.append(f"AiApiController revived response-index coordination: {forbidden}")

wiring = (SRC / "runtime/AppWiring.cpp").read_text(errors="replace")
for needle in (
    "ctx.setHealthUseCase(healthUseCase)",
    "HealthController::setUseCase(healthUseCase.get())",
    "ctx.setChannelAdminUseCase(channelAdmin)",
    "ChannelController::setUseCase(channelAdmin.get())",
    "ctx.setMetricsUseCase(metricsUseCase)",
    "MetricsController::setUseCase(metricsUseCase.get())",
    "ctx.setRetoolWorkspaceAdminUseCase(workspaceAdmin)",
    "RetoolWorkspaceController::setUseCase(workspaceAdmin.get())",
    "AccountController::setUseCase(accountAdmin.get())",
    "std::make_shared<AiApiUseCase>(",
    "ctx.setAiApiUseCase(aiApiUseCase)",
    "AiApiController::setUseCase(aiApiUseCase.get())",
    "AiApiController::setUseCase(nullptr)",
):
    if needle not in wiring:
        errors.append(f"runtime controller wiring missing: {needle}")

for port in set(management.values()):
    if not (SRC / port).exists():
        errors.append(f"controller use-case port missing: src/{port}")

if errors:
    print("P5-W3 controller use-case injection gate FAIL")
    for error in errors:
        print(f"  {error}")
    raise SystemExit(FAIL)

print("PASS P5-W3: every business Controller depends only on one injected use case")
