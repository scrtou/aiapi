#!/usr/bin/env python3
"""Exact P2 gate for retired concrete providers and preserved public protocols."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def source_text() -> str:
    chunks: list[str] = []
    for suffix in ("*.h", "*.hpp", "*.cc", "*.cpp"):
        for path in SRC.rglob(suffix):
            chunks.append(path.read_text(encoding="utf-8"))
    return "\n".join(chunks)


all_source = source_text()
controller = (SRC / "controllers/AiApiController.h").read_text(encoding="utf-8")
channels = (SRC / "channelManager/channelManager.cpp").read_text(encoding="utf-8")
account_write_sources = (
    SRC / "accountManager/AccountSelector.cpp",
    SRC / "accountManager/AccountRegistrationWorkflow.cpp",
)
accounts = "\n".join(path.read_text(encoding="utf-8") for path in account_write_sources)

for relative in ("src/apipoint/openai", "src/apipoint/nexosapi"):
    require(not (ROOT / relative).exists(), f"retired implementation directory still exists: {relative}")

require("OpenAiProvider" not in all_source, "OpenAiProvider symbol remains in src")
require(
    re.search(r"IMPLEMENT_RUNTIME\s*\(\s*(?:openai|nexosapi)\b", all_source) is None,
    "retired provider factory registration remains",
)
require("nexosapi" not in channels and "openai" not in channels,
        "retired key remains in ChannelManager active/default whitelist")
require('return name == "chaynsapi" || name == "retoolapi";' in channels,
        "ChannelManager built-in whitelist is not exactly chaynsapi/retoolapi")
require("retired_provider::isRetiredProviderKey(apiName)" in accounts,
        "account write/registration workflows do not guard retired provider apiName")
require("retired_provider::isRetiredProviderKey(accountinfo.apiName)" in accounts,
        "account add/update workflow does not guard retired provider accountinfo.apiName")

legacy_routes = {
    ('retiredNexos', '/nexosapi/v1/chat/completions', 'Post'),
    ('retiredNexos', '/nexosapi/v1/models', 'Get'),
    ('retiredNexos', '/nexosapi/v1/account/quota', 'Get'),
    ('retiredNexos', '/nexosapi/v1/responses', 'Post'),
    ('retiredNexosWithId', '/nexosapi/v1/responses/{1}', 'Get'),
    ('retiredNexosWithId', '/nexosapi/v1/responses/{1}', 'Delete'),
}
for handler, route, method in legacy_routes:
    signature = (
        f'ADD_METHOD_TO(AiApiController::{handler}, "{route}", drogon::{method})'
    )
    require(signature in controller, f"legacy route is not mapped to tombstone: {method} {route}")

for provider in ("chaynsapi", "retoolapi"):
    required_routes = (
        ("chaynsapichat", f"/{provider}/v1/chat/completions", "Post"),
        ("responsesCreate", f"/{provider}/v1/responses", "Post"),
        ("responsesGet", f"/{provider}/v1/responses/{{1}}", "Get"),
        ("responsesDelete", f"/{provider}/v1/responses/{{1}}", "Delete"),
    )
    for handler, route, method in required_routes:
        signature = f'ADD_METHOD_TO(AiApiController::{handler}, "{route}", drogon::{method}'
        require(signature in controller, f"OpenAI-compatible route was lost: {method} {route}")

config_path = ROOT / "config.example.json"
config = json.loads(config_path.read_text(encoding="utf-8"))
custom = config.get("custom_config", {})
providers = custom.get("providers", {})
limits = custom.get("outbound_limits", {})
for key in ("openai", "nexos", "nexosapi"):
    require(key not in providers, f"config example retains custom_config.providers.{key}")
for key in ("openai", "nexosapi"):
    require(key not in limits, f"config example retains custom_config.outbound_limits.{key}")
for array_key in ("login_service_urls", "regist_service_urls", "downstream_service_api_keys"):
    for index, item in enumerate(custom.get(array_key, [])):
        require(item.get("name") not in {"openai", "nexosapi"},
                f"config example retains custom_config.{array_key}[{index}].name")

require(any("openai_resource_uuid" in p.read_text(encoding="utf-8")
            for p in SRC.rglob("*.h")) or
        any("openai_resource_uuid" in p.read_text(encoding="utf-8")
            for p in SRC.rglob("*.cpp")),
        "legal Retool openai_resource_uuid business field was removed")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("PASS retired-provider gate: implementations/factories removed, tombstones and compatible routes preserved")
