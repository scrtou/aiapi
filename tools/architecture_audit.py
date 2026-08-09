#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Architecture ratchet for RFC-001 (v3).

The rules are deliberately structural:

R1  A free function exported by tooling/actionProtocol competes with a
    GenerationService member of the same name.
R2  A high-fan-in production header has no *declared test owner*.
    A test owns a header only when it directly includes it or declares
    ``// ARCH_TESTS: path/from/src/Header.h``.  This is test reachability,
    not line/branch coverage.  Transitive includes never count as coverage.
R3  A production function is longer than the configured line threshold.

The script scans .h/.hpp/.cpp/.cc consistently.  R2 intentionally does not
try to infer runtime execution from include graphs; real coverage belongs to
gcov/llvm-cov and behaviour contracts.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
TEST = os.path.join(SRC, "test")
TEST_CMAKE = os.path.join(TEST, "CMakeLists.txt")

HEADER_EXTS = (".h", ".hpp")
SOURCE_EXTS = (".cpp", ".cc")
PRODUCTION_EXTS = HEADER_EXTS + SOURCE_EXTS
R2_FANIN = 2
R3_LIMIT = 200

INCDIRS = [
    SRC,
    os.path.join(SRC, "apiManager"),
    os.path.join(SRC, "apipoint"),
    os.path.join(SRC, "apipoint", "chaynsapi"),
    os.path.join(SRC, "sessionManager"),
    os.path.join(SRC, "controllers"),
    os.path.join(SRC, "controllers", "sinks"),
    os.path.join(SRC, "tools"),
]


def walk(base, exts):
    for directory, dirs, files in os.walk(base):
        dirs[:] = [d for d in dirs if d not in {"build", ".deps"}]
        for filename in files:
            if filename.endswith(exts):
                yield os.path.join(directory, filename)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as stream:
        return stream.read()


def nlines(path):
    return read(path).count("\n") + 1


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def cmake_block(name):
    match = re.search(r"set\(\s*" + name + r"\s(.*?)\)", read(TEST_CMAKE), re.S)
    if not match:
        return []
    output = []
    for line in match.group(1).splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        line = line.replace("${CMAKE_CURRENT_SOURCE_DIR}", TEST)
        path = line if os.path.isabs(line) else os.path.join(TEST, line)
        path = os.path.normpath(path)
        if path.endswith(SOURCE_EXTS):
            output.append(path)
    return output


INC_RE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.M)
OWNER_RE = re.compile(r"^\s*//\s*ARCH_TESTS:\s*([^\s]+)\s*$", re.M)
DROGON_TEST = re.compile(r"^\s*DROGON_TEST\s*\(", re.M)
GTEST = re.compile(r"^\s*TEST(?:_F|_P)?\s*\(", re.M)
ASSERTION = re.compile(r"\b(?:CHECK|REQUIRE|ASSERT_[A-Z]+|EXPECT_[A-Z]+)\s*\(")


def resolve(inc, origin):
    candidates = [os.path.join(os.path.dirname(origin), inc)]
    candidates.extend(os.path.join(directory, inc) for directory in INCDIRS)
    for candidate in candidates:
        candidate = os.path.normpath(candidate)
        if os.path.isfile(candidate) and os.path.commonpath([candidate, SRC]) == SRC:
            return candidate
    return None


FUNC_DEF = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_:<>,&*~\s]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", re.M
)
MEMBER = re.compile(r"\bGenerationService::([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def rule_r1():
    members = set()
    for path in walk(SRC, PRODUCTION_EXTS):
        if path.startswith(TEST + os.sep):
            continue
        members.update(MEMBER.findall(read(path)))

    hits = []
    for directory, _, files in os.walk(SRC):
        if os.path.basename(directory) not in {"tooling", "actionProtocol"}:
            continue
        for filename in files:
            if not filename.endswith(HEADER_EXTS):
                continue
            path = os.path.join(directory, filename)
            for name in set(FUNC_DEF.findall(read(path))):
                if name in members:
                    hits.append({"func": name, "header": rel(path)})
    return sorted(hits, key=lambda item: (item["func"], item["header"]))


def test_owners():
    owners = {}
    diagnostics = []
    for test in cmake_block("TEST_SOURCES"):
        if not os.path.isfile(test):
            continue
        body = read(test)
        cases = len(DROGON_TEST.findall(body)) + len(GTEST.findall(body))
        assertions = len(ASSERTION.findall(body))
        declared = set()
        if cases and assertions:
            for inc in INC_RE.findall(body):
                header = resolve(inc, test)
                if header and header.endswith(HEADER_EXTS) and not header.startswith(TEST + os.sep):
                    declared.add(header)
            for marker in OWNER_RE.findall(body):
                header = os.path.normpath(os.path.join(SRC, marker))
                if os.path.isfile(header) and header.startswith(SRC + os.sep):
                    declared.add(header)
            for header in declared:
                owners.setdefault(header, set()).add(test)
        diagnostics.append(
            {
                "file": os.path.basename(test),
                "cases": cases,
                "assertions": assertions,
                "owned_headers": len(declared),
            }
        )
    return owners, diagnostics


def implementation_for(header):
    stem = os.path.splitext(header)[0]
    for extension in SOURCE_EXTS:
        candidate = stem + extension
        if os.path.isfile(candidate):
            return candidate
    return None


def rule_r2():
    owners, diagnostics = test_owners()
    linked = set(cmake_block("PROJECT_SOURCES"))
    fanin = {}
    for consumer in walk(SRC, PRODUCTION_EXTS):
        if consumer.startswith(TEST + os.sep):
            continue
        for inc in set(INC_RE.findall(read(consumer))):
            header = resolve(inc, consumer)
            if header and header != consumer and not header.startswith(TEST + os.sep):
                fanin.setdefault(header, set()).add(consumer)

    hits = []
    for header, consumers in fanin.items():
        if len(consumers) < R2_FANIN or owners.get(header):
            continue
        impl = implementation_for(header)
        hits.append(
            {
                "header": rel(header),
                "fanin": len(consumers),
                "test_owners": 0,
                "linked": bool(impl and impl in linked),
                "impl_lines": nlines(impl) if impl else 0,
            }
        )
    hits.sort(key=lambda item: (-item["fanin"], -item["impl_lines"], item["header"]))
    return hits, owners, fanin, diagnostics


TOP = re.compile(r"^[A-Za-z_~][A-Za-z0-9_:<>,&*~\s]*\(|^(?:static|inline|namespace)\b")


def rule_r3():
    hits = []
    for path in walk(SRC, SOURCE_EXTS):
        if path.startswith(TEST + os.sep):
            continue
        lines = read(path).splitlines()
        starts = [index for index, line in enumerate(lines, 1) if TOP.match(line)]
        starts.append(len(lines) + 1)
        for start, end in zip(starts, starts[1:]):
            if end - start > R3_LIMIT:
                hits.append(
                    {"file": rel(path), "start": start, "end": end - 1, "lines": end - start}
                )
    return sorted(hits, key=lambda item: (-item["lines"], item["file"]))


def git_metadata():
    def run(*args):
        result = subprocess.run(
            ["git", *args], cwd=ROOT, capture_output=True, text=True, check=False
        )
        return result.stdout.strip() if result.returncode == 0 else "unknown"

    return {
        "commit": run("rev-parse", "--short", "HEAD"),
        "dirty": bool(run("status", "--porcelain")),
    }


def collect():
    r2, owners, fanin, tests = rule_r2()
    rules = {"R1": rule_r1(), "R2": r2, "R3": rule_r3()}
    production_sources = [
        path for path in walk(SRC, SOURCE_EXTS) if not path.startswith(TEST + os.sep)
    ]
    test_sources = [path for path in cmake_block("TEST_SOURCES") if os.path.isfile(path)]
    linked_sources = [path for path in cmake_block("PROJECT_SOURCES") if os.path.isfile(path)]
    return {
        "schema_version": 3,
        "generated_at": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "git": git_metadata(),
        "definitions": {
            "R1": "tooling free-function/member-name competition",
            "R2": "high-fan-in header without a direct or explicit test owner; not runtime coverage",
            "R3": "production function longer than 200 lines",
        },
        "counts": {
            "R1": len(rules["R1"]),
            "R2": len(rules["R2"]),
            "R3": len(rules["R3"]),
            "R3_lines": sum(item["lines"] for item in rules["R3"]),
            "production_sources": len(production_sources),
            "linked_production_sources": len(linked_sources),
            "test_sources": len(test_sources),
            "test_cases": sum(item["cases"] for item in tests),
            "assertions": sum(item["assertions"] for item in tests),
        },
        "rules": rules,
        "diagnostics": {
            "owned_headers": len(owners),
            "fanin_headers": len(fanin),
            "tests": tests,
        },
    }


def baseline_counts(payload):
    if "counts" in payload:
        return payload["counts"]
    return {key: int(payload.get(key, 0)) for key in ("R1", "R2", "R3")}


def selftest(payload):
    errors = []
    counts = payload["counts"]
    if counts["test_sources"] == 0 or counts["test_cases"] == 0:
        errors.append("registered tests were not parsed")
    if not any(path.endswith(".cc") for path in walk(SRC, SOURCE_EXTS)):
        errors.append(".cc fixture missing; extension coverage cannot be verified")
    if not any(item["owned_headers"] > 0 for item in payload["diagnostics"]["tests"]):
        errors.append("no test declares a direct production-header owner")

    owners, _ = test_owners()
    direct = set()
    explicit = set()
    for test in cmake_block("TEST_SOURCES"):
        if not os.path.isfile(test):
            continue
        body = read(test)
        cases = len(DROGON_TEST.findall(body)) + len(GTEST.findall(body))
        assertions = len(ASSERTION.findall(body))
        if not cases or not assertions:
            continue
        for inc in INC_RE.findall(body):
            header = resolve(inc, test)
            if header and header.startswith(SRC + os.sep) and not header.startswith(TEST + os.sep):
                direct.add(header)
        for marker in OWNER_RE.findall(body):
            header = os.path.normpath(os.path.join(SRC, marker))
            if os.path.isfile(header):
                explicit.add(header)
    if not direct.issubset(set(owners)):
        errors.append("a directly included production header was not assigned to its test")

    # Negative invariant: headers are never owned merely because they are transitive.
    transitive_only = set(owners) - direct - explicit
    if transitive_only:
        errors.append("transitive-only headers unexpectedly became test owners")

    print("architecture_audit selftest v3")
    print("  extensions: .h .hpp .cpp .cc")
    print("  registered tests: %d, cases: %d" % (counts["test_sources"], counts["test_cases"]))
    print("  direct/explicit owned headers: %d" % payload["diagnostics"]["owned_headers"])
    if errors:
        for error in errors:
            print("  FAIL:", error)
        return 2
    print("  PASS: direct ownership is used; transitive includes are not called coverage")
    return 0


def print_report(payload):
    counts = payload["counts"]
    print("architecture_audit v3")
    print("  R1 name competition: %d" % counts["R1"])
    print("  R2 high-fan-in headers without test owner: %d" % counts["R2"])
    print("  R3 functions > %d lines: %d (%d lines)" % (R3_LIMIT, counts["R3"], counts["R3_lines"]))
    print(
        "  sources/tests/cases/assertions: %d/%d/%d/%d"
        % (
            counts["production_sources"],
            counts["test_sources"],
            counts["test_cases"],
            counts["assertions"],
        )
    )
    for item in payload["rules"]["R2"]:
        print("    R2 fanin=%-3d %s" % (item["fanin"], item["header"]))
    for item in payload["rules"]["R3"]:
        print("    R3 %4d %s:%d-%d" % (item["lines"], item["file"], item["start"], item["end"]))


def render_markdown(payload):
    """Render the human view from the exact same payload as the JSON baseline."""
    counts = payload["counts"]
    git = payload["git"]
    dirty = "dirty（实施快照，不是可复现发布基线）" if git["dirty"] else "clean"
    lines = [
        "# 架构审计基线",
        "",
        "> 本文件由 `tools/architecture_audit.py` 生成，请勿手改数字。机器可读真值为 [`audit-baseline.json`](./audit-baseline.json)。",
        "",
        "## 1. 快照",
        "",
        "| 项 | 值 |",
        "|---|---|",
        "| schema | v%d |" % payload["schema_version"],
        "| 生成时间 | `%s` |" % payload["generated_at"],
        "| 基础 commit | `%s` |" % git["commit"],
        "| 工作区 | **%s** |" % dirty,
        "| 扫描扩展名 | `.h/.hpp/.cpp/.cc` |",
        "",
        "发布 tag 使用的基线必须来自 `git.dirty=false`。dirty 基线只允许作为当前实施快照。",
        "",
        "## 2. 指标",
        "",
        "| 规则 | 当前值 | 含义 |",
        "|---|---:|---|",
        "| R1 | **%d** | tooling/actionProtocol 自由函数与 GenerationService 成员同名竞争 |" % counts["R1"],
        "| R2 | **%d** | fan-in ≥2 且无直接/显式 test owner 的生产头；**不是运行时覆盖** |" % counts["R2"],
        "| R3 | **%d** | `.cpp/.cc` 中超过 200 行的函数候选 |" % counts["R3"],
        "| R3 行数 | **%d** | R3 候选总行数 |" % counts["R3_lines"],
        "",
        "| 规模项 | 当前值 |",
        "|---|---:|",
        "| 生产翻译单元 | %d |" % counts["production_sources"],
        "| 测试直接编译的生产源 | %d |" % counts["linked_production_sources"],
        "| 测试源 | %d |" % counts["test_sources"],
        "| 测试用例 | %d |" % counts["test_cases"],
        "| 静态断言宏计数 | %d |" % counts["assertions"],
        "",
        "规模项用于发现 CMake/注册漂移，不作为质量 KPI。",
        "",
        "## 3. R2 语义",
        "",
        "owner 只来自有用例和断言的测试源直接 include，或 `// ARCH_TESTS: path/from/src/Header.h`。传递 include 不产生 owner。即使有 owner，也不能证明运行时执行；真实覆盖使用 gcov/llvm-cov。",
        "",
        "v3 口径与旧版不可比较，当前值是新棘轮起点。无行为头可以通过规则修正或 ADR 排除，不应为清零写无意义断言。",
        "",
        "## 4. R2 明细",
        "",
        "| fan-in | impl 行 | 已进测试链接 | 头文件 |",
        "|---:|---:|:---:|---|",
    ]
    for item in payload["rules"]["R2"]:
        lines.append(
            "| %d | %d | %s | `%s` |"
            % (item["fanin"], item["impl_lines"], "是" if item["linked"] else "否", item["header"])
        )
    lines.extend(
        [
            "",
            "## 5. R3 明细",
            "",
            "| 行数 | 位置 |",
            "|---:|---|",
        ]
    )
    for item in payload["rules"]["R3"]:
        lines.append(
            "| %d | `%s:%d-%d` |"
            % (item["lines"], item["file"], item["start"], item["end"])
        )
    lines.extend(
        [
            "",
            "## 6. 更新",
            "",
            "```bash",
            "python3 tools/architecture_audit.py --selftest",
            "python3 tools/architecture_audit.py \\",
            "  --write-baseline doc/adr/audits/audit-baseline.json \\",
            "  --write-markdown doc/adr/audits/architecture-baseline.md",
            "```",
            "",
            "回归检查：",
            "",
            "```bash",
            "python3 tools/architecture_audit.py --baseline doc/adr/audits/audit-baseline.json",
            "```",
            "",
        ]
    )
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--baseline")
    parser.add_argument("--write-baseline")
    parser.add_argument("--write-markdown")
    args = parser.parse_args()

    payload = collect()
    if args.selftest:
        return selftest(payload)
    if args.write_baseline:
        with open(args.write_baseline, "w", encoding="utf-8") as stream:
            json.dump(payload, stream, indent=2, ensure_ascii=False)
            stream.write("\n")
    if args.write_markdown:
        with open(args.write_markdown, "w", encoding="utf-8") as stream:
            stream.write(render_markdown(payload))
    if args.json:
        print(json.dumps(payload, indent=2, ensure_ascii=False))
    else:
        print_report(payload)

    if args.baseline and os.path.isfile(args.baseline):
        with open(args.baseline, encoding="utf-8") as stream:
            baseline = baseline_counts(json.load(stream))
        regressions = []
        for rule in ("R1", "R2", "R3"):
            if payload["counts"][rule] > baseline.get(rule, 0):
                regressions.append(
                    "%s %d>%d" % (rule, payload["counts"][rule], baseline.get(rule, 0))
                )
        if regressions:
            print("REGRESSION: " + ", ".join(regressions))
            return 1
        print("ratchet PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
