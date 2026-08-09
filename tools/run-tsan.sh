#!/usr/bin/env bash
# ThreadSanitizer 本地回归脚本（不进 CI，理由见 doc 与本文件末尾说明）。
#
# 用法:
#   tools/run-tsan.sh              # 配置(如需) + 构建 + 全量单测 + 停机专项 + 信号夹具
#   tools/run-tsan.sh --repeat 10  # 指定停机专项/夹具的重复次数(默认 5)
#
# 退出码: 0 全绿; 1 有测试失败或出现未被抑制的 TSan 竞态告警。
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR=build-tsan
REPEAT=5
while [ $# -gt 0 ]; do
  case "$1" in
    --repeat) REPEAT="$2"; shift 2 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

# 抑制文件必须存在：它承载的是「这些竞态是第三方库的、已判定」的结论，
# 缺了它会退化成一堆无人复核的噪声告警，等于没跑。
SUPP="$REPO_ROOT/tsan.supp"
[ -f "$SUPP" ] || { echo "缺少 $SUPP" >&2; exit 2; }
export TSAN_OPTIONS="suppressions=$SUPP:halt_on_error=0:exitcode=0:history_size=7"

# 超时上限：停机类用例正常不到 1s，超过这个量级就是卡住了，
# 宁可判失败也不能让回归脚本无限悬停。
TIMEOUT_ALL=${TIMEOUT_ALL:-900}
TIMEOUT_CASE=${TIMEOUT_CASE:-120}

fail=0
LOGDIR="$(mktemp -d)"
trap 'rm -rf "$LOGDIR"' EXIT

# 判据说明：exitcode=0 让 TSan 不再改写进程退出码，这样退出码只反映
# 测试断言的成败；竞态与否单独用日志里的 WARNING 计数判断。两个信号分开，
# 才能区分「用例挂了」和「用例过了但有竞态」。
warn_count() { grep -c 'WARNING: ThreadSanitizer' "$1" 2>/dev/null || true; }

echo "===== [1/4] 配置并构建 $BUILD_DIR ====="
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -O1 -g -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" > "$LOGDIR/cfg.log" 2>&1 \
    || { echo '配置失败:'; tail -30 "$LOGDIR/cfg.log"; exit 1; }
fi
cmake --build "$BUILD_DIR" -j"$(nproc)" > "$LOGDIR/build.log" 2>&1 \
  || { echo '构建失败:'; tail -30 "$LOGDIR/build.log"; exit 1; }
echo '  ok  构建完成'

TEST_BIN="$BUILD_DIR/src/test/aiapi_test"
FIXTURE_BIN="$BUILD_DIR/src/test/aiapi_shutdown_signal_fixture"

echo "===== [2/4] 全量单测 ====="
timeout "$TIMEOUT_ALL" "$TEST_BIN" > "$LOGDIR/all.log" 2>&1; rc=$?
[ "$rc" -eq 124 ] && echo "  FAIL 全量单测超时 ${TIMEOUT_ALL}s（此处挂起通常=丢失唤醒，看日志最后一个用例）"
sed 's/\x1b\[[0-9;]*m//g' "$LOGDIR/all.log" | grep -E 'All tests passed|assertions:|test cases:' | tail -3
w=$(warn_count "$LOGDIR/all.log")
[ "$rc" -ne 0 ] && { echo "  FAIL 全量单测退出码 $rc"; sed 's/\x1b\[[0-9;]*m//g' "$LOGDIR/all.log" | grep -A6 'FAILED:' | head -40; fail=1; }
# signal-unsafe 是 Drogon 信号路径固有的（operator new via queueInLoop），
# 不是数据竞争，也不在我们能修的范围内，故只对 data race 计失败。
races=$(grep -c 'WARNING: ThreadSanitizer: data race' "$LOGDIR/all.log" 2>/dev/null || true)
echo "  告警合计=$w 其中 data race=$races"
[ "${races:-0}" -gt 0 ] && { echo '  FAIL 出现未被抑制的数据竞争'; grep -A25 'data race' "$LOGDIR/all.log" | head -60; fail=1; }

echo "===== [3/4] 停机专项 x$REPEAT ====="
for t in BackgroundTaskQueue_ShutdownIsIrreversible \
         BackgroundTaskQueue_ShutdownDrainsBacklogAndRejectsLateWork \
         BackgroundTaskQueue_ShutdownWaitsForRunningTask \
         ShutdownWorkers_LongWaitsAreInterruptibleAndStopsAreIdempotent; do
  bad=0
  for r in $(seq 1 "$REPEAT"); do
    timeout "$TIMEOUT_CASE" "$TEST_BIN" -r "$t" > "$LOGDIR/$t.$r.log" 2>&1; trc=$?
    [ "$trc" -eq 124 ] && { echo "  FAIL $t 第 $r 次超时 ${TIMEOUT_CASE}s"; bad=1; }
    [ "$trc" -ne 0 ] && bad=1
    [ "$(grep -c 'data race' "$LOGDIR/$t.$r.log" 2>/dev/null || true)" -gt 0 ] && bad=1
  done
  if [ "$bad" -eq 0 ]; then echo "  ok  $t ($REPEAT/$REPEAT)"; else echo "  FAIL $t"; fail=1; fi
done

echo "===== [4/4] 信号夹具 x$REPEAT ====="
# 顺序标记是这道检查的全部意义：仅看退出码会把「排空顺序错乱但仍退出 0」
# 判成通过。READY 由 loop 定时器发出，保证 SIGTERM handler 已安装。
MARKERS='READY REAPER ACCOUNTS SESSION QUEUE EXIT'
for r in $(seq 1 "$REPEAT"); do
  log="$LOGDIR/fixture.$r.log"
  "$FIXTURE_BIN" > "$log" 2>&1 &
  pid=$!
  for _ in $(seq 1 200); do grep -q READY "$log" 2>/dev/null && break; sleep 0.05; done
  sleep 0.2
  kill -TERM "$pid" 2>/dev/null
  # 看门狗：夹具若在停机路径上死锁，wait 会永远不返回，
  # 那就从"发现问题的脚本"变成"自己也挂住的脚本"。
  ( sleep "$TIMEOUT_CASE"; kill -9 "$pid" 2>/dev/null ) & wd=$!
  wait "$pid"; frc=$?
  kill -9 "$wd" 2>/dev/null; wait "$wd" 2>/dev/null
  missing=""
  for m in $MARKERS; do grep -qx "$m" "$log" || missing="$missing $m"; done
  if [ "$frc" -eq 0 ] && [ -z "$missing" ]; then
    echo "  ok  run$r rc=0 标记齐全"
  else
    echo "  FAIL run$r rc=$frc 缺失标记:${missing:- 无}"; fail=1
  fi
done

echo
[ "$fail" -eq 0 ] && echo '===== TSan 全绿 =====' || echo '===== TSan 存在失败项 ====='
exit $fail
