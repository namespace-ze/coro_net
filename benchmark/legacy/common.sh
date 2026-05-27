#!/usr/bin/env bash
# 共享变量与函数：所有 benchmark/run_*.sh 都 source 这一份
# -----------------------------------------------------------------------------
# 模式：
#   单机模式（默认）：SERVER_HOST 未设或为 127.0.0.1 → 脚本自动 start/stop server
#   双机模式：SERVER_HOST=<remote_ip> → 脚本不动 server，假定用户在另一台机器上
#             手动启动了 ./build/example/echo_server_coro <BENCH_PORT> <workers>
# -----------------------------------------------------------------------------

set -eo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(cd "$BENCH_DIR/.." && pwd)"
RESULTS_DIR="$BENCH_DIR/results"
SERVER_BIN="$PROJ_DIR/build/example/echo_server_coro"
CLIENT_BIN="$BENCH_DIR/echo_client"

# 压测端口与目标主机
BENCH_PORT="${BENCH_PORT:-18002}"
SERVER_HOST="${SERVER_HOST:-127.0.0.1}"

mkdir -p "$RESULTS_DIR"

# 是否本地 server
is_local() {
    case "$SERVER_HOST" in
        127.0.0.1|localhost|::1) return 0;;
        *) return 1;;
    esac
}

# 启动服务端，参数：workers
# 单机模式：实际启动 + 返回 pid
# 双机模式：仅检查可达性 + 返回 "remote" 标记
start_server() {
    local workers="$1"
    [ -x "$CLIENT_BIN" ] || {
        echo "ERROR: $CLIENT_BIN not built" >&2
        echo "  build: g++-13 -O2 -std=c++20 -pthread $BENCH_DIR/echo_client.cc -o $CLIENT_BIN" >&2
        exit 1
    }

    if is_local; then
        [ -x "$SERVER_BIN" ] || { echo "ERROR: $SERVER_BIN not built" >&2; exit 1; }
        local logfile="$RESULTS_DIR/server-w${workers}-$(date +%s).log"
        "$SERVER_BIN" "$BENCH_PORT" "$workers" > "$logfile" 2>&1 &
        local pid=$!
        for _ in 1 2 3 4 5 6; do
            sleep 0.5
            if ss -tln 2>/dev/null | grep -q ":$BENCH_PORT "; then
                echo "$pid"
                return 0
            fi
        done
        echo "ERROR: server failed to listen on :$BENCH_PORT" >&2
        kill -9 "$pid" 2>/dev/null
        return 1
    else
        # 远程：检查 TCP 可达
        if ! timeout 3 bash -c "</dev/tcp/$SERVER_HOST/$BENCH_PORT" 2>/dev/null; then
            echo "ERROR: cannot reach $SERVER_HOST:$BENCH_PORT" >&2
            echo "" >&2
            echo "  On the SERVER machine, start:" >&2
            echo "    ./build/example/echo_server_coro $BENCH_PORT $workers" >&2
            echo "  (or use the helper: ./benchmark/server_runner.sh $workers)" >&2
            return 1
        fi
        # 返回 "remote" 字面量做 sentinel
        echo "remote"
        return 0
    fi
}

stop_server() {
    local pid="$1"
    [ -n "$pid" ] || return 0
    if [ "$pid" = "remote" ]; then
        return 0   # 远程：不管
    fi
    kill -INT "$pid" 2>/dev/null || true
    sleep 1
    kill -9 "$pid" 2>/dev/null || true
    rm -f "$PROJ_DIR"/echo_server_coro.*.log
}

# 跑客户端：threads conns duration msg_size [warmup]
run_client() {
    local t="$1" c="$2" d="$3" m="$4" warm="${5:-3}"
    "$CLIENT_BIN" --host "$SERVER_HOST" --port "$BENCH_PORT" \
        --threads "$t" --connections "$c" \
        --duration "$d" --warmup "$warm" \
        --msg-size "$m"
}

# 在 run_*.sh 顶部打印当前模式，让用户确认
print_mode_banner() {
    if is_local; then
        echo "[mode] LOCAL  ($SERVER_HOST:$BENCH_PORT, server managed by script)"
    else
        echo "[mode] REMOTE ($SERVER_HOST:$BENCH_PORT, server must be running on remote)"
    fi
}
