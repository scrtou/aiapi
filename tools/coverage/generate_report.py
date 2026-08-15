#!/usr/bin/env python3
"""Generate the aiapi production coverage baseline from GCC gcov JSON.

Production implementations are compiled once by their canonical formal
library target and reused by the test executables.  The collector reads those
libraries' gcda files plus the ``aiapi_test`` gcda
files (needed for production inline/header code instantiated by tests).  It
deliberately excludes the unexecuted production ``aiapi`` executable target.
Files absent from the test-linked object graph are reported as
``not_instrumented`` instead of being silently omitted or assigned a made-up
denominator.
"""

import argparse
import gzip
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
SOURCE_SUFFIXES = {".cpp", ".cc"}
PRODUCTION_SUFFIXES = SOURCE_SUFFIXES | {".h", ".hpp"}


TARGETS = [
    {
        "name": "Chayns provider generate/upstream request",
        "file": "src/infrastructure/provider/chayns/chaynsapi.cpp",
        "functions": ["chaynsapi::doGenerate(", "chaynsapi::sendWithinContext("],
    },
    {
        "name": "Generation pipeline request/provider/commit",
        "file": "src/sessionManager/core/GenerationPipeline.cpp",
        "functions": [
            "generation::GenerationPipeline::run(",
            "generation::GenerationPipeline::execute(",
            "generation::GenerationPipeline::invokeProvider(",
        ],
    },
    {
        "name": "Generation response decode/normalize/emit",
        "file": "src/sessionManager/core/GenerationResponsePipeline.cpp",
        "functions": [
            "generation::GenerationResponsePipeline::emit(",
        ],
    },
    {
        "name": "Generation tool bridge request transform",
        "file": "src/sessionManager/tooling/ToolDefinitionEncoder.cpp",
        "functions": [
            "toolcall::transformRequestForToolBridge(",
        ],
    },
    {
        "name": "Generation tool argument normalization",
        "file": "src/sessionManager/tooling/ToolCallNormalizer.cpp",
        "functions": [
            "toolcall::normalizeToolCallArguments(",
        ],
    },
    {
        "name": "Generation forced tool fallback",
        "file": "src/sessionManager/tooling/ForcedToolCallGenerator.cpp",
        "functions": [
            "toolcall::generateForcedToolCall(",
        ],
    },
    {
        "name": "Account selection/invalidation/pool rebuild",
        "file": "src/accountManager/AccountSelector.cpp",
        "functions": [
            "AccountManager::getAccount(",
            "AccountManager::getEligibleAccount(",
            "AccountManager::setStatusTokenStatus(",
            "AccountManager::rebuildPoolLocked(",
            "AccountManager::loadAccount(",
            "AccountManager::addAccountbyPost(",
            "AccountManager::updateAccount(",
            "AccountManager::deleteAccountbyPost(",
        ],
    },
    {
        "name": "Account registration rollback",
        "file": "src/accountManager/AccountRegistrationWorkflow.cpp",
        "functions": [
            "AccountManager::rollbackWaitingAccount(",
            "AccountManager::autoRegisterAccount(",
        ],
    },
    {
        "name": "Account token refresh",
        "file": "src/accountManager/AccountTokenWorkflow.cpp",
        "functions": [
            "AccountManager::checkToken(",
        ],
    },
    {
        "name": "BackgroundTaskQueue shutdown/drain",
        "file": "src/infrastructure/executor/BackgroundTaskQueue.h",
        "functions": [
            "BackgroundTaskQueue::enqueue(",
            "BackgroundTaskQueue::shutdown(",
            "BackgroundTaskQueue::workerLoop(",
        ],
    },
    {
        "name": "Account worker interrupt/join",
        "file": "src/accountManager/AccountWorkers.cpp",
        "functions": ["AccountManager::stopBackgroundThreads("],
    },
    {
        "name": "Session cleaner interrupt/join",
        "file": "src/sessionManager/core/Session.cpp",
        "functions": ["chatSession::stopClearExpiredSession("],
    },
    {
        "name": "Chayns reaper interrupt/join",
        "file": "src/infrastructure/provider/chayns/chaynsThreadReaper.cpp",
        "functions": ["chaynsThreadReaper::stop("],
    },
    {
        "name": "Streaming disconnect boundary",
        "file": "src/controllers/sinks/IoLoopResponseStream.h",
        "functions": [
            "IoLoopResponseStream::send(",
            "IoLoopResponseStream::sendInLoop(",
            "IoLoopResponseStream::closeInLoop(",
        ],
    },
    {
        "name": "HTTP Controller Chat/Responses route edge",
        "file": "src/controllers/AiApiController.cc",
        "functions": [
            "AiApiController::chaynsapichat(",
            "AiApiController::responsesCreate(",
        ],
    },
    {
        "name": "Retool workflow/agent upstream paths",
        "file": "src/infrastructure/provider/retool/retoolapi.cpp",
        "functions": ["retoolapi::requestWorkflow(", "retoolapi::requestAgent("],
    },
    {
        "name": "Process shutdown orchestration after Drogon run",
        "file": "src/runtime/AppContext.cpp",
        "functions": ["lifecycle::AppContext::shutdown("],
    },
]


def posix_relative(path):
    """Return a repository-relative POSIX path, or None outside the repo."""
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return None


def production_files():
    files = []
    for path in SRC.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        if SRC / "test" in path.parents:
            continue
        files.append(posix_relative(path))
    return sorted(files)


def empty_file_record():
    return {"lines": {}, "branches": {}, "functions": {}}


def merge_gcov_file(records, item):
    path = Path(item["file"])
    relative = posix_relative(path)
    if relative is None or not relative.startswith("src/"):
        return
    if relative.startswith("src/test/") or path.suffix not in PRODUCTION_SUFFIXES:
        return

    record = records.setdefault(relative, empty_file_record())
    for line in item.get("lines", []):
        number = int(line["line_number"])
        record["lines"][number] = record["lines"].get(number, 0) + int(line["count"])
        for index, branch in enumerate(line.get("branches", [])):
            # A header can be emitted by several translation units.  Branch
            # ordinal within a source line is stable in one compiler build;
            # union it and add execution counts rather than inflating the
            # denominator once per translation unit.
            key = (number, index)
            record["branches"][key] = record["branches"].get(key, 0) + int(branch["count"])

    for function in item.get("functions", []):
        name = function.get("demangled_name") or function.get("name", "")
        key = (name, int(function.get("start_line", 0)), int(function.get("end_line", 0)))
        existing = record["functions"].setdefault(
            key,
            {
                "name": name,
                "start_line": key[1],
                "end_line": key[2],
                "execution_count": 0,
                "blocks": 0,
                "blocks_executed": 0,
            },
        )
        existing["execution_count"] += int(function.get("execution_count", 0))
        existing["blocks"] = max(existing["blocks"], int(function.get("blocks", 0)))
        existing["blocks_executed"] = max(
            existing["blocks_executed"], int(function.get("blocks_executed", 0))
        )


def collect(build_dir, gcov):
    target_markers = (
        os.sep + "aiapi_platform.dir" + os.sep,
        os.sep + "aiapi_domain.dir" + os.sep,
        os.sep + "aiapi_application.dir" + os.sep,
        os.sep + "aiapi_infrastructure.dir" + os.sep,
        os.sep + "aiapi_transport.dir" + os.sep,
        os.sep + "aiapi_runtime.dir" + os.sep,
        os.sep + "aiapi_test.dir" + os.sep,
    )
    data_files = sorted(
        path
        for path in build_dir.rglob("*.gcda")
        if any(marker in str(path) for marker in target_markers)
    )
    if not data_files:
        raise RuntimeError(
            "no production-library/aiapi_test gcda files found; build with "
            "AIAPI_ENABLE_COVERAGE=ON and run ctest first"
        )

    records = {}
    with tempfile.TemporaryDirectory(prefix="aiapi-gcov-") as temporary:
        temporary = Path(temporary)
        for index, data_file in enumerate(data_files):
            output_dir = temporary / str(index)
            output_dir.mkdir()
            completed = subprocess.run(
                [gcov, "-j", "-b", "-c", str(data_file.resolve())],
                cwd=output_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if completed.returncode:
                raise RuntimeError(
                    "gcov failed for {}:\n{}{}".format(
                        data_file, completed.stdout, completed.stderr
                    )
                )
            json_files = list(output_dir.glob("*.gcov.json.gz"))
            if len(json_files) != 1:
                raise RuntimeError(
                    "expected one gcov JSON for {}, got {}".format(data_file, len(json_files))
                )
            with gzip.open(json_files[0], "rt", encoding="utf-8") as stream:
                payload = json.load(stream)
            for item in payload.get("files", []):
                merge_gcov_file(records, item)
    return data_files, records


def metrics(record, start=None, end=None):
    if record is None:
        return None
    lines = {
        number: count
        for number, count in record["lines"].items()
        if (start is None or number >= start) and (end is None or number <= end)
    }
    branches = {
        key: count
        for key, count in record["branches"].items()
        if (start is None or key[0] >= start) and (end is None or key[0] <= end)
    }
    return {
        "lines": len(lines),
        "lines_covered": sum(count > 0 for count in lines.values()),
        "branches": len(branches),
        "branches_covered": sum(count > 0 for count in branches.values()),
    }


def ratio(covered, total):
    return None if not total else round(100.0 * covered / total, 2)


def add_ratios(item):
    item["line_percent"] = ratio(item["lines_covered"], item["lines"])
    item["branch_percent"] = ratio(item["branches_covered"], item["branches"])
    return item


def serialise_file(relative, record):
    result = add_ratios(metrics(record))
    result.update(
        {
            "file": relative,
            "state": "executed" if result["lines_covered"] else "instrumented_not_executed",
        }
    )
    return result


def target_result(target, records):
    record = records.get(target["file"])
    result = {
        "name": target["name"],
        "file": target["file"],
        "requested_functions": target["functions"],
        "functions": [],
    }
    if record is None:
        result["state"] = "not_instrumented"
        result["reason"] = "production file is absent from the test-linked object graph"
        return result

    result["state"] = "instrumented"
    result.update(add_ratios(metrics(record)))
    functions = list(record["functions"].values())
    for requested in target["functions"]:
        matches = [
            function
            for function in functions
            if requested in function["name"]
            and "::{lambda" not in function["name"]
            # GCC also emits local-class methods under the enclosing function
            # name (for example ``foo(...)::Guard::~Guard``). They are not an
            # additional match for the requested production entry point.
            and ")::" not in function["name"]
        ]
        if not matches:
            result["functions"].append(
                {"requested": requested, "state": "symbol_not_found"}
            )
            continue
        for function in matches:
            item = {
                "requested": requested,
                "name": function["name"],
                "state": "executed" if function["execution_count"] else "not_executed",
                "execution_count": function["execution_count"],
                "start_line": function["start_line"],
                "end_line": function["end_line"],
                "blocks": function["blocks"],
                "blocks_executed": function["blocks_executed"],
            }
            item.update(
                add_ratios(
                    metrics(record, function["start_line"], function["end_line"])
                )
            )
            result["functions"].append(item)
    return result


def make_report(build_dir, gcov, data_files, records):
    sources = production_files()
    source_records = [
        serialise_file(relative, records[relative])
        for relative in sources
        if relative in records
    ]
    missing = [relative for relative in sources if relative not in records]

    total = {"lines": 0, "lines_covered": 0, "branches": 0, "branches_covered": 0}
    for item in source_records:
        for key in total:
            total[key] += item[key]
    add_ratios(total)

    version = subprocess.run(
        [gcov, "--version"], stdout=subprocess.PIPE, text=True, check=True
    ).stdout.splitlines()[0]
    return {
        "schema_version": 1,
        "scope": "production code exercised through test-linked production objects",
        "provenance": {
            "repository": str(ROOT),
            "build_dir": str(build_dir),
            "gcov": version,
            "gcda_files": len(data_files),
            "note": (
                "Production implementations are compiled once by their canonical "
                "aiapi_* production library; tests link those targets and provide "
                "execution evidence for pulled objects."
            ),
        },
        "summary": {
            "production_implementation_files": len(sources),
            "instrumented_implementation_files": len(source_records),
            "not_instrumented_implementation_files": len(missing),
            **total,
        },
        "implementation_files": source_records,
        "not_instrumented": missing,
        "targets": [target_result(target, records) for target in TARGETS],
    }


def pct(value):
    return "n/a" if value is None else "{:.2f}%".format(value)


def markdown(report):
    summary = report["summary"]
    lines = [
        "# 运行时覆盖机器报告",
        "",
        "> 此文件由 `tools/coverage/generate_report.py` 生成。不要手工修改数字。",
        "",
        "## 1. 口径",
        "",
        "- 数据源：测试进程运行产生的 `aiapi_*` production libraries/`aiapi_test` gcda；只统计仓库 `src/` 下生产文件。",
        "- 生产 `.cpp/.cc` 只由其 canonical production library 编译一次；测试链接这些 target，不维护第二份生产源清单。",
        "- 未进入测试链接对象图的文件标为 `not_instrumented`，不伪造可执行行分母，也不算入百分比。",
        "- 分支采用 GCC gcov 分支口径，包含编译器生成的异常处理分支。",
        "",
        "## 2. 摘要",
        "",
        "| 项 | 值 |",
        "|---|---:|",
        "| production 实现文件 | {} |".format(summary["production_implementation_files"]),
        "| 已编入并 instrument 的实现文件 | {} |".format(summary["instrumented_implementation_files"]),
        "| 未进入测试链接对象图的实现文件 | {} |".format(summary["not_instrumented_implementation_files"]),
        "| 行覆盖（仅已 instrument 实现） | {}/{} ({}) |".format(
            summary["lines_covered"], summary["lines"], pct(summary["line_percent"])
        ),
        "| 分支覆盖（仅已 instrument 实现） | {}/{} ({}) |".format(
            summary["branches_covered"], summary["branches"], pct(summary["branch_percent"])
        ),
        "| gcda 文件 | {} |".format(report["provenance"]["gcda_files"]),
        "| 工具 | `{}` |".format(report["provenance"]["gcov"]),
        "",
        "## 3. 高风险路径",
        "",
        "| 路径 | 文件 | 状态 | 行 | 分支 |",
        "|---|---|---|---:|---:|",
    ]
    for target in report["targets"]:
        if target["state"] == "not_instrumented":
            line_value = branch_value = "n/a"
        else:
            line_value = "{}/{} ({})".format(
                target["lines_covered"], target["lines"], pct(target["line_percent"])
            )
            branch_value = "{}/{} ({})".format(
                target["branches_covered"], target["branches"], pct(target["branch_percent"])
            )
        lines.append(
            "| {} | `{}` | `{}` | {} | {} |".format(
                target["name"], target["file"], target["state"], line_value, branch_value
            )
        )

    lines.extend(
        [
            "",
            "### 目标函数执行证据",
            "",
            "| 路径 | 函数 | 状态 | 执行次数 | 行 | 分支 |",
            "|---|---|---|---:|---:|---:|",
        ]
    )
    for target in report["targets"]:
        if target["state"] == "not_instrumented":
            for function in target["requested_functions"]:
                lines.append(
                    "| {} | `{}` | `not_instrumented` | n/a | n/a | n/a |".format(
                        target["name"], function
                    )
                )
            continue
        for function in target["functions"]:
            if function["state"] == "symbol_not_found":
                lines.append(
                    "| {} | `{}` | `symbol_not_found` | n/a | n/a | n/a |".format(
                        target["name"], function["requested"]
                    )
                )
                continue
            lines.append(
                "| {} | `{}` | `{}` | {} | {}/{} ({}) | {}/{} ({}) |".format(
                    target["name"],
                    function["requested"],
                    function["state"],
                    function["execution_count"],
                    function["lines_covered"],
                    function["lines"],
                    pct(function["line_percent"]),
                    function["branches_covered"],
                    function["branches"],
                    pct(function["branch_percent"]),
                )
            )

    lines.extend(
        [
            "",
            "## 4. 未进入测试链接对象图的生产实现",
            "",
            "这些文件没有运行时覆盖证据，是后续 fixture/characterization 的输入：",
            "",
        ]
    )
    lines.extend("- `{}`".format(path) for path in report["not_instrumented"])
    lines.extend(
        [
            "",
            "## 5. 已 instrument 实现明细",
            "",
            "| 文件 | 状态 | 行 | 分支 |",
            "|---|---|---:|---:|",
        ]
    )
    for item in report["implementation_files"]:
        lines.append(
            "| `{}` | `{}` | {}/{} ({}) | {}/{} ({}) |".format(
                item["file"],
                item["state"],
                item["lines_covered"],
                item["lines"],
                pct(item["line_percent"]),
                item["branches_covered"],
                item["branches"],
                pct(item["branch_percent"]),
            )
        )
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build-coverage")
    parser.add_argument("--json-output", default="build-coverage/coverage-report.json")
    parser.add_argument(
        "--markdown-output",
        default="doc/adr/work-products/P01-runtime-coverage-report.md",
    )
    parser.add_argument("--gcov", default="gcov")
    args = parser.parse_args()

    build_dir = (ROOT / args.build_dir).resolve() if not Path(args.build_dir).is_absolute() else Path(args.build_dir)
    if not build_dir.is_dir():
        parser.error("build directory does not exist: {}".format(build_dir))
    gcov = shutil.which(args.gcov)
    if gcov is None:
        parser.error("gcov executable not found: {}".format(args.gcov))

    try:
        data_files, records = collect(build_dir, gcov)
        report = make_report(build_dir, gcov, data_files, records)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("coverage report failed: {}".format(error), file=sys.stderr)
        return 2

    json_output = ROOT / args.json_output if not Path(args.json_output).is_absolute() else Path(args.json_output)
    markdown_output = ROOT / args.markdown_output if not Path(args.markdown_output).is_absolute() else Path(args.markdown_output)
    json_output.parent.mkdir(parents=True, exist_ok=True)
    markdown_output.parent.mkdir(parents=True, exist_ok=True)
    with json_output.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, sort_keys=True)
        stream.write("\n")
    markdown_output.write_text(markdown(report), encoding="utf-8")
    print("wrote {}".format(json_output))
    print("wrote {}".format(markdown_output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
