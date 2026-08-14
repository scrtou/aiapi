#!/usr/bin/env python3
"""P6-W1 ratchet for Result/Error and the thin ProviderBase NVI boundary."""

from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
FAIL = 4
errors: list[str] = []


def source(relative: str) -> str:
    path = SRC / relative
    if not path.is_file():
        errors.append(f"missing P6-W1 foundation file: src/{relative}")
        return ""
    return path.read_text(errors="replace")


error_code = source("platform/result/ErrorCode.h")
error_model = source("platform/result/Error.h")
result = source("platform/result/Result.h")
deadline = source("platform/Deadline.h")
cancellation = source("platform/Cancellation.h")
request = source("domain/model/ProviderRequest.h")
response = source("domain/model/ProviderResponse.h")
context = source("domain/model/ProviderCallContext.h")
chat_port = source("domain/port/IChatProvider.h")
base_header = source("infrastructure/provider/ProviderBase.h")
base_cpp = source("infrastructure/provider/ProviderBase.cpp")
factory = source("infrastructure/provider/ProductionProviderFactory.h")
generation_event = source("sessionManager/contracts/GenerationEvent.h")
legacy_errors = source("sessionManager/core/Errors.h")
test_cmake = source("test/CMakeLists.txt")

for name in (
    "None", "BadRequest", "Unauthorized", "Forbidden", "NotFound", "Conflict",
    "RateLimited", "Timeout", "ProviderError", "Internal", "Cancelled",
):
    if name not in error_code:
        errors.append(f"platform ErrorCode missing stable value: {name}")

for source_name, text in {
    "sessionManager/contracts/GenerationEvent.h": generation_event,
    "sessionManager/core/Errors.h": legacy_errors,
}.items():
    if "using ErrorCode = platform::ErrorCode" not in text:
        errors.append(f"{source_name} did not alias the single platform ErrorCode")
    if "platform::defaultHttpStatus" not in text:
        errors.append(f"{source_name} kept an independent ErrorCode -> HTTP mapping")

for needle in (
    "std::variant<Value, Error>",
    "class [[nodiscard]] Result",
    "class [[nodiscard]] Result<void>",
    "static Result success(",
    "static Result failure(",
    "Result::value() called on failure",
):
    if needle not in result:
        errors.append(f"Result contract missing: {needle}")

for needle in (
    "upstreamHttpStatus",
    "std::string providerCode",
    "std::string detail",
    "defaultHttpStatus(code)",
):
    if needle not in error_model:
        errors.append(f"platform Error contract missing: {needle}")

for needle in (
    "using Deadline = std::chrono::steady_clock::time_point",
    "deadlineExpired",
    "remainingUntil",
):
    if needle not in deadline:
        errors.append(f"deadline contract missing: {needle}")

for needle in (
    "class CancellationToken",
    "CancellationToken token() const",
    "bool isCancelled() const",
):
    if needle not in cancellation:
        errors.append(f"read-only cancellation token contract missing: {needle}")

try:
    token_declaration = cancellation.split("class CancellationToken", 1)[1].split(
        "class CancellationSource", 1)[0]
except IndexError:
    token_declaration = ""
if "std::shared_ptr<cancellation_detail::State>" not in token_declaration:
    errors.append("CancellationToken must retain shared cancellation state after source destruction")
if re.search(r"\b(?:request|cancel)\s*\(", token_declaration):
    errors.append("CancellationToken must remain read-only; cancellation belongs to CancellationSource")

for text_name, text in {
    "platform/result/ErrorCode.h": error_code,
    "platform/result/Error.h": error_model,
    "platform/result/Result.h": result,
    "domain/model/ProviderRequest.h": request,
    "domain/model/ProviderResponse.h": response,
    "domain/model/ProviderCallContext.h": context,
    "domain/port/IChatProvider.h": chat_port,
}.items():
    if re.search(r"(?:json/json\.h|drogon/|Json::|drogon::)", text):
        errors.append(f"{text_name} leaked JsonCpp/Drogon into the P6 port boundary")

for text_name, text in {
    "domain/model/ProviderRequest.h": request,
    "domain/model/ProviderResponse.h": response,
    "domain/model/ProviderCallContext.h": context,
    "domain/port/IChatProvider.h": chat_port,
}.items():
    # Comments are expected to name the legacy types during the migration; the
    # contract itself must not include or declare them.
    code = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    if re.search(r"\b(?:session_st|APIinterface)\b", code):
        errors.append(f"{text_name} leaked legacy session/APIinterface into the P6 provider port")

for needle in (
    "const platform::CancellationToken& cancellation",
    "platform::Deadline deadline",
    "deadlineExceeded()",
):
    if needle not in context:
        errors.append(f"ProviderCallContext missing request control: {needle}")

for needle in (
    "platform::Result<ProviderResponse> generate(",
    "const ProviderRequest& request",
    "ProviderCallContext& context",
    "ProviderCapabilities capabilities() const noexcept",
):
    if needle not in chat_port:
        errors.append(f"IChatProvider contract missing: {needle}")

if not re.search(r"class\s+ProviderBase\s*:\s*public\s+IChatProvider", base_header):
    errors.append("ProviderBase must inherit IChatProvider")
if not re.search(
    r"platform::Result<ProviderResponse>\s+generate\s*\(.*?\)\s*final\s*;",
    base_header,
    re.S,
):
    errors.append("ProviderBase generate() must be the final NVI entrypoint")
for needle in (
    "doGenerate(",
    "providerName() const noexcept",
    "FailureObserver",
):
    if needle not in base_header:
        errors.append(f"ProviderBase contract missing: {needle}")
for needle in (
    "context.isCancelled()",
    "context.deadlineExceeded()",
    "catch (const std::exception& exception)",
    "reportFailure(error)",
):
    if needle not in base_cpp:
        errors.append(f"ProviderBase implementation missing boundary behavior: {needle}")

if not re.search(
    r"static_assert\s*\(\s*std::is_base_of_v\s*<\s*ProviderBase\s*,\s*ProviderT\s*>",
    factory,
):
    errors.append("production provider factory lost its ProviderBase static assertion")

# A fake in src/test may implement the port directly.  Production code may not
# bypass ProviderBase as the new P6 registry is introduced in W2/W3.  Scan all
# production C++ sources and accept both class/struct spellings (including
# `final`) so a cosmetic declaration change cannot bypass the ratchet.
direct_port_implementation = re.compile(
    r"\b(?:class|struct)\s+\w+(?:\s+final)?\s*:\s*public\s+(?:provider::)?IChatProvider\b",
    re.S,
)
for suffix in ("*.h", "*.hpp", "*.cpp", "*.cc"):
    for path in SRC.rglob(suffix):
        if "test" in path.parts or path == SRC / "infrastructure/provider/ProviderBase.h":
            continue
        text = path.read_text(errors="replace")
        if direct_port_implementation.search(text):
            errors.append(
                f"{path.relative_to(ROOT)} directly implements IChatProvider; production providers must use ProviderBase")


def run_nodiscard_compiler_probe() -> None:
    """Prove that a discarded Result is rejected under the CI warning policy.

    The production build deliberately does not use global -Werror, so merely
    spelling [[nodiscard]] is insufficient evidence that this invariant is
    enforceable.  This header-only probe needs no Drogon/DB dependencies and
    keeps the policy localized to the Result boundary.
    """
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler or not shutil.which(compiler[0]):
        errors.append("P6-W1 nodiscard probe could not locate the C++ compiler from CXX/c++")
        return

    with tempfile.TemporaryDirectory(prefix="aiapi-result-nodiscard-") as tmp:
        tmp_path = Path(tmp)
        positive = tmp_path / "positive.cpp"
        discarded = tmp_path / "discarded.cpp"
        source_prefix = """#include <platform/result/Result.h>

platform::Result<int> makeResult()
{
    return platform::Result<int>::success(42);
}
"""
        positive.write_text(source_prefix + """
int main()
{
    const auto result = makeResult();
    return result.ok() ? 0 : 1;
}
""")
        discarded.write_text(source_prefix + """
int main()
{
    makeResult();
    return 0;
}
""")
        command_prefix = compiler + [
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror=unused-result",
            "-I",
            str(SRC),
            "-fsyntax-only",
        ]
        try:
            valid = subprocess.run(
                command_prefix + [str(positive)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
            if valid.returncode != 0:
                errors.append(
                    "P6-W1 nodiscard probe valid Result use failed to compile:\n"
                    + valid.stdout.strip())
                return
            ignored = subprocess.run(
                command_prefix + [str(discarded)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
        except OSError as exception:
            errors.append(f"P6-W1 nodiscard compiler probe could not run: {exception}")
            return
        if ignored.returncode == 0:
            errors.append(
                "P6-W1 nodiscard probe accepted a discarded Result under -Werror=unused-result")


if not errors:
    run_nodiscard_compiler_probe()

for test_source in ("test_result.cpp", "test_provider_base.cpp"):
    if test_source not in test_cmake:
        errors.append(f"P6-W1 contract test not registered: src/test/{test_source}")

if errors:
    print("P6-W1 Result/ProviderBase foundation gate FAIL")
    for error in errors:
        print(f"  {error}")
    raise SystemExit(FAIL)

print("PASS P6-W1: Result/Error, ProviderCallContext, NVI, and production inheritance gate are present")
