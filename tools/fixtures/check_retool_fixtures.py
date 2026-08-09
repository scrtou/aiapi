#!/usr/bin/env python3
"""Validate committed Retool characterization fixtures without network access."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIR = ROOT / "src" / "test" / "fixtures" / "retool"
EXPECTED = {
    "workflow-success.json",
    "agent-success.json",
    "http-errors.json",
    "invalid-json.json",
}
JWT = re.compile(r"\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b")
EMAIL = re.compile(r"\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b", re.IGNORECASE)
SECRET_FIELD = re.compile(r"^(access.?token|xsrf.?token|cookie|authorization|password|api.?key)$", re.IGNORECASE)


def walk(value: Any, path: str = "$") -> Iterable[tuple[str, Any]]:
    yield path, value
    if isinstance(value, dict):
        for key, child in value.items():
            yield from walk(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from walk(child, f"{path}[{index}]")


def validate(path: Path) -> list[str]:
    errors: list[str] = []
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        return [f"invalid JSON: {exc}"]
    if value.get("fixture_version") != 1:
        errors.append("fixture_version must be 1")
    if value.get("source") != "synthetic-characterization":
        errors.append("source must explicitly be synthetic-characterization")
    raw = path.read_text(encoding="utf-8")
    if JWT.search(raw):
        errors.append("JWT-like value found")
    if EMAIL.search(raw):
        errors.append("email-like value found")
    for location, item in walk(value):
        if isinstance(item, dict):
            for key, child in item.items():
                if SECRET_FIELD.match(key) and key != "headers_present" and child not in (None, "", [], {}):
                    errors.append(f"secret-bearing field committed at {location}.{key}")
        if isinstance(item, str) and item.startswith(("http://", "https://")) and not item.startswith(("http://localhost", "https://localhost")):
            host = item.split("/", 3)[2].split(":", 1)[0]
            if not host.endswith(".invalid"):
                errors.append(f"non-reserved host at {location}: {host}")
    return errors


def main() -> int:
    actual = {p.name for p in FIXTURE_DIR.glob("*.json")}
    errors: list[str] = []
    if actual != EXPECTED:
        errors.append(f"fixture set mismatch: expected={sorted(EXPECTED)}, actual={sorted(actual)}")
    for path in sorted(FIXTURE_DIR.glob("*.json")):
        for error in validate(path):
            errors.append(f"{path.relative_to(ROOT)}: {error}")
    if errors:
        print("Retool fixture safety: FAIL", file=sys.stderr)
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Retool fixture safety: {len(actual)} synthetic files PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
