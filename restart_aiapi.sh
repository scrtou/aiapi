#!/usr/bin/env bash
# aiapi 安全重启脚本（脱离当前终端，后台常驻）
set -euo pipefail
REPO_ROOT="/home/vps/code/aiapi"
BUILD_DIR="$REPO_ROOT/build"
# src/CMakeLists.txt explicitly emits the server here.  Do not fall back to
# build/src/aiapi: that path is a stale pre-output-unification artifact.
BIN="$BUILD_DIR/aiapi"
LEGACY_BIN="$BUILD_DIR/src/aiapi"
OUT_LOG="$BUILD_DIR/logs/aiapi.stdout.log"
PORT=55555

mkdir -p "$BUILD_DIR/logs"

# Refuse to take down a healthy service for an unbuilt/stale canonical target.
# This catches exactly the old situation where build/aiapi predates the newer
# build/src/aiapi output.  Reconfigure and rebuild before invoking this script.
if [ ! -x "$BIN" ]; then
  echo "未找到规范运行产物: $BIN"
  echo "请先执行: cmake -S $REPO_ROOT -B $BUILD_DIR && cmake --build $BUILD_DIR --target aiapi"
  exit 1
fi
if [ -x "$LEGACY_BIN" ] && [ "$LEGACY_BIN" -nt "$BIN" ]; then
  echo "规范运行产物比遗留 build/src/aiapi 旧，拒绝重启以免启动旧版本。"
  echo "请先执行: cmake -S $REPO_ROOT -B $BUILD_DIR && cmake --build $BUILD_DIR --target aiapi"
  exit 1
fi

OLD_PIDS=$(pgrep -x aiapi || true)
if [ -n "$OLD_PIDS" ]; then
  echo "[1/4] 停止旧进程: $OLD_PIDS"
  kill $OLD_PIDS 2>/dev/null || true
  for i in $(seq 1 20); do
    pgrep -x aiapi >/dev/null || break
    sleep 0.5
  done
  if pgrep -x aiapi >/dev/null; then
    echo "[1/4] 优雅退出超时，强制结束"
    pkill -9 -x aiapi 2>/dev/null || true
    sleep 1
  fi
else
  echo "[1/4] 无运行中的 aiapi 进程"
fi

# CMake 仍会保留 build/src/ 作为子目录的构建元数据，但不应再保留第二个
# 可执行文件。只在旧进程停止且规范产物已通过上方校验后删除它。
if [ -e "$LEGACY_BIN" ]; then
  rm -f "$LEGACY_BIN"
  echo "[1/4] 已清理遗留运行产物: $LEGACY_BIN"
fi

echo "[2/4] 启动新进程"
cd "$BUILD_DIR" || exit 1
setsid nohup "$BIN" >>"$OUT_LOG" 2>&1 < /dev/null &
sleep 2

echo "[3/4] 进程状态"
pgrep -ax aiapi || { echo "启动失败，请查看 $OUT_LOG"; tail -n 30 "$OUT_LOG"; exit 1; }

echo "[4/4] 端口监听"
ss -lntp 2>/dev/null | grep ":$PORT" || echo "警告: 端口 $PORT 未监听"
echo "完成。传输层实时日志: tail -f $BUILD_DIR/logs/aiapi.log"
echo "应用层实时日志:   tail -f $OUT_LOG"
echo "应用层持久日志:   tail -f $BUILD_DIR/logs/aiapi.application.log"
