#!/usr/bin/env bash
# aiapi 安全重启脚本（脱离当前终端，后台常驻）
set -u
BUILD_DIR="/home/vps/code/aiapi/build"
BIN="$BUILD_DIR/aiapi"
OUT_LOG="$BUILD_DIR/logs/aiapi.stdout.log"
PORT=55555

mkdir -p "$BUILD_DIR/logs"

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

echo "[2/4] 启动新进程"
cd "$BUILD_DIR" || exit 1
setsid nohup "$BIN" >>"$OUT_LOG" 2>&1 < /dev/null &
sleep 2

echo "[3/4] 进程状态"
pgrep -ax aiapi || { echo "启动失败，请查看 $OUT_LOG"; tail -n 30 "$OUT_LOG"; exit 1; }

echo "[4/4] 端口监听"
ss -lntp 2>/dev/null | grep ":$PORT" || echo "警告: 端口 $PORT 未监听"
echo "完成。实时日志: tail -f $BUILD_DIR/logs/aiapi.log"
