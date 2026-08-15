#!/usr/bin/env python3
"""P4-W3 停机 deadline 语义门禁（失败退出码 4）。

`AppContext` 把同一个绝对截止时间传给 owner 只是形参层面的保证；owner 若随后
调用无参 stop/shutdown，宽限期仍会在最后一跳丢失。本门禁检查五个持线程 owner
都把 deadline 传入限时汇合路径，并且实现确实使用 `platform::joinUntil`。

这是结构性门禁，不能替代能让 worker 真正超时的行为测试；后者由 P4-W3 的
ErrorStats/BackgroundTaskQueue 测试覆盖。
"""
import re
import sys

FAIL = 4


def read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        return None


def check_file(path, text, patterns, problems):
    if text is None:
        problems.append(f"{path} 不存在，无法验证停机 deadline")
        return
    for label, pattern in patterns:
        if not re.search(pattern, text, flags=re.S):
            problems.append(f"{path} 缺少 {label}: {pattern}")


def main():
    problems = []
    app_context = read("src/runtime/AppContext.h")
    wiring = read("src/runtime/AppWiring.cpp")
    account_h = read("src/accountManager/accountManager.h")
    # P7-W2 leaves AccountManager.cpp as the composition/configuration
    # facade.  The actual four-worker lifecycle (and therefore the final
    # deadline-aware join) is owned by AccountWorkers.cpp; checking the old
    # facade would make this gate demand the very monolith the slice removed.
    account_workers = read("src/accountManager/AccountWorkers.cpp")
    reaper_h = read("src/infrastructure/provider/chayns/chaynsThreadReaper.h")
    reaper_cpp = read("src/infrastructure/provider/chayns/chaynsThreadReaper.cpp")
    session_h = read("src/sessionManager/core/Session.h")
    session_cpp = read("src/sessionManager/core/Session.cpp")
    queue_h = read("src/infrastructure/executor/BackgroundTaskQueue.h")
    stats_h = read("src/metrics/ErrorStatsService.h")
    stats_cpp = read("src/metrics/ErrorStatsService.cpp")

    check_file("src/runtime/AppContext.h", app_context,
               [("RuntimeOwner 绝对 deadline",
                 r"std::function\s*<\s*void\s*\(\s*std::chrono::steady_clock::time_point")],
               problems)

    # Wiring checks deliberately require the deadline in the actual call expression,
    # not merely in a preceding log statement.
    check_file("src/runtime/AppWiring.cpp", wiring, [
        ("context-owned BackgroundTaskQueue::shutdown(deadline)",
         r"\bqueue\s*->\s*shutdown\s*\(\s*deadline\s*\)"),
        ("AccountManager::stopBackgroundThreads(deadline)",
         r"\baccounts\s*->\s*stopBackgroundThreads\s*\(\s*deadline\s*\)"),
        ("chaynsThreadReaper::stop(deadline)",
         r"\breaper\s*->\s*stop\s*\(\s*deadline\s*\)"),
        ("chatSession::stopClearExpiredSession(deadline)",
         r"stopClearExpiredSession\s*\(\s*deadline\s*\)"),
        ("ErrorStatsService::shutdown(deadline)",
         r"\berrorStats\s*->\s*shutdown\s*\(\s*deadline\s*\)"),
    ], problems)

    check_file("src/accountManager/accountManager.h", account_h,
               [("限时 stop 声明", r"stopBackgroundThreads\s*\(\s*std::chrono::steady_clock::time_point\s+deadline\s*\)")], problems)
    check_file("src/accountManager/AccountWorkers.cpp", account_workers,
               [("四个账号 worker 的 joinUntil", r"joinUntil\s*\("),
                ("账号 worker completion", r"tokenCheckDone_|tokenUpdateDone_|accountCountDone_|accountTypeDone_")], problems)
    check_file("src/infrastructure/provider/chayns/chaynsThreadReaper.h", reaper_h,
               [("限时 stop 声明", r"stop\s*\(\s*std::chrono::steady_clock::time_point\s+deadline\s*\)"),
                ("reaper completion", r"workerDone_")], problems)
    check_file("src/infrastructure/provider/chayns/chaynsThreadReaper.cpp", reaper_cpp,
               [("reaper joinUntil", r"joinUntil\s*\("),
                ("reaper completion signal", r"workerDone_\s*=\s*std::make_shared")], problems)
    check_file("src/sessionManager/core/Session.cpp", session_cpp,
               [("session cleaner joinUntil", r"joinUntil\s*\(")], problems)
    check_file("src/infrastructure/executor/BackgroundTaskQueue.h", queue_h,
               [("队列限时 shutdown", r"shutdown\s*\(\s*std::chrono::steady_clock::time_point\s+deadline\)"),
                ("队列 joinUntil", r"joinUntil\s*\(")], problems)
    check_file("src/metrics/ErrorStatsService.h", stats_h,
               [("ErrorStats 限时 shutdown", r"shutdown\s*\(\s*std::chrono::steady_clock::time_point\s+deadline\)")], problems)
    check_file("src/metrics/ErrorStatsService.cpp", stats_cpp,
               [("ErrorStats joinUntil", r"joinUntil\s*\(")], problems)

    if problems:
        print("停机 deadline 门禁未通过：")
        for problem in problems:
            print("  FAIL " + problem)
        return FAIL

    print("OK P4-W3 五个 owner 均传播绝对 deadline 并使用限时汇合")
    return 0


if __name__ == "__main__":
    sys.exit(main())
