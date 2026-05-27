#!/usr/bin/env bash
# =============================================================================
# server_runner.sh —— 在 server 机器上启停服务端的便利脚本
# =============================================================================
# 用法（在 server 机器上）：
#
#   ./benchmark/server_runner.sh <workers> [port]
#
# 行为：
#   1. 找到正在跑的 echo_server_coro 进程，发 SIGINT 优雅退出（最多等 3 秒）
#   2. 启动新的 echo_server_coro <port> <workers>
#   3. 前台运行，便于看输出 / Ctrl-C 退出
#
# 典型流程（双机压测）：
#   server 端: ./benchmark/server_runner.sh 4
#   client 端: SERVER_HOST=10.0.0.5 bash benchmark/run_qps.sh
# =============================================================================

set -e

WORKERS="${1:-4}"
PORT="${2:-18002}"
SERVER_BIN="$(dirname "$(realpath "$0")")/../build/example/echo_server_coro"

if [ ! -x "$SERVER_BIN" ]; then
    echo "ERROR: $SERVER_BIN not found. Build first:" >&2
    echo "  cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
    exit 1
fi

# 杀掉旧实例
old_pid=$(pgrep -f "echo_server_coro $PORT " 2>/dev/null || true)
if [ -n "$old_pid" ]; then
    echo "[runner] killing old server (pid $old_pid)..."
    kill -INT "$old_pid" 2>/dev/null || true
    for _ in 1 2 3 4 5 6; do
        sleep 0.5
        kill -0 "$old_pid" 2>/dev/null || break
    done
    kill -9 "$old_pid" 2>/dev/null || true
fi

# 检查端口已释放
if ss -tln 2>/dev/null | grep -q ":$PORT "; then
    echo "WARNING: :$PORT still occupied" >&2
fi

# 提升 fd 上限（仅本进程的子继承）
ulimit -n 1000000 2>/dev/null || true

echo "[runner] starting echo_server_coro $PORT $WORKERS"
echo "[runner] Ctrl-C to stop. Server logs to ./echo_server_coro.*.log"
echo ""

# exec：替换当前 shell，让 SIGINT 直接到 server 进程
exec "$SERVER_BIN" "$PORT" "$WORKERS"
