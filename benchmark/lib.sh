#!/usr/bin/env bash
# =============================================================================
# benchmark/lib.sh —— 公用函数：工具检查 / tcpkali 调用 / 输出解析 / CSV 追加
# =============================================================================
# 所有 run_*.sh 都 source 这一份。设计目标：
#   - 单一工具：tcpkali（v0.4+）。netperf 需要 netserver，无法测 echo_server，已弃用
#   - 参数化：通过环境变量控制 HOST/PORT/DURATION/ROUNDS/MSG_SIZE/OUT_DIR
#   - 不安装任何东西：缺工具就报错退出，提示用户自己装
#   - 单/双机统一：HOST=127.0.0.1 时脚本自管 server；其它 IP 时假定远程已起好
# =============================================================================

set -eo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(cd "$BENCH_DIR/.." && pwd)"
SERVER_BIN="$PROJ_DIR/build/example/echo_server_coro"

# ---- 可调参数（环境变量覆盖） ----
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-18002}"
DURATION="${DURATION:-10s}"         # 单次测试时长（如 10s / 30s / 60s）
ROUNDS="${ROUNDS:-1}"               # 每个数据点重复轮数（用于取中位）
MSG_SIZE="${MSG_SIZE:-64}"          # 消息大小（字节，含 latency marker）
TCPKALI_WORKERS="${TCPKALI_WORKERS:-4}"  # tcpkali 自身线程数

# ---- 输出目录 ----
if [ -z "${OUT_DIR:-}" ]; then
    ts=$(date +%Y%m%d-%H%M%S)
    OUT_DIR="$BENCH_DIR/results/local/$ts"
fi
mkdir -p "$OUT_DIR"

# 单/双机模式判定
is_local() {
    case "$HOST" in
        127.0.0.1|localhost|::1) return 0;;
        *) return 1;;
    esac
}

# 启动前置检查：必须有 tcpkali 二进制
require_tools() {
    if ! command -v tcpkali >/dev/null 2>&1; then
        cat >&2 <<EOF
ERROR: tcpkali not found in PATH.

Install (user responsibility — not done by scripts):
  Debian/Ubuntu (newer): apt install tcpkali
  Older / RHEL family : build from source
                        https://github.com/satori-com/tcpkali
EOF
        exit 1
    fi
}

# 启动 server（仅本地模式）。参数：workers。返回：pid 或 "remote"
start_server() {
    local workers="$1"
    if is_local; then
        [ -x "$SERVER_BIN" ] || {
            echo "ERROR: $SERVER_BIN not built. Run: cmake --build build -j" >&2
            exit 1
        }
        local logfile="$OUT_DIR/server-w${workers}-$(date +%s).log"
        "$SERVER_BIN" "$PORT" "$workers" > "$logfile" 2>&1 &
        local pid=$!
        for _ in 1 2 3 4 5 6; do
            sleep 0.5
            ss -tln 2>/dev/null | grep -q ":$PORT " && { echo "$pid"; return 0; }
        done
        echo "ERROR: server failed to listen on :$PORT" >&2
        kill -9 "$pid" 2>/dev/null
        return 1
    else
        if ! timeout 3 bash -c "</dev/tcp/$HOST/$PORT" 2>/dev/null; then
            cat >&2 <<EOF
ERROR: cannot reach $HOST:$PORT
On the server machine, start:
  ./build/example/echo_server_coro $PORT $workers
(or use: ./benchmark/server_runner.sh $workers $PORT)
EOF
            return 1
        fi
        echo "remote"
        return 0
    fi
}

stop_server() {
    local pid="$1"
    [ -n "$pid" ] || return 0
    [ "$pid" = "remote" ] && return 0
    kill -INT "$pid" 2>/dev/null || true
    sleep 1
    kill -9 "$pid" 2>/dev/null || true
    rm -f "$PROJ_DIR"/echo_server_coro.*.log
}

# 构造定长 message：首字节为 latency marker '@'，其余补 'x'
# 参数：size（字节数）
make_message() {
    local size="$1"
    local pad=$((size - 1))
    printf '@%*s' "$pad" '' | tr ' ' 'x'
}

# 跑一次 tcpkali，输出 raw 文本到 stdout
# 参数：conns duration [extra_args...]
# 注意：tcpkali 默认 --connect-rate=100，对大连接数会撞 duration。这里按 conns*10
#       (但不低于 1000) 自动放宽。调用方再传 --connect-rate 会覆盖。
# 错误处理：tcpkali 非零退出（如 TIME-WAIT 端口耗尽）不传播为 fatal——
#         返回时 stdout 含原始输出，parse_tcpkali 在缺数据时填 0。
#         调用方判失败：检查 parse_tcpkali 输出的 dur_s 是否为 0。
run_tcpkali() {
    local conns="$1" dur="$2"; shift 2
    local msg cr
    msg=$(make_message "$MSG_SIZE")
    cr=$(( conns * 10 ))
    [ "$cr" -lt 1000 ] && cr=1000
    # 外层 timeout：tcpkali --duration 设了上限，但极端情况（端口耗尽）下
    # 可能卡在连接阶段，timeout 兜底防止脚本永远挂住
    # duration "10s" → 取数字 10，再 +20s 余量
    local dur_num="${dur%[smh]}"
    local hard_timeout=$(( dur_num + 30 ))
    # set +e 局部，避免 lib.sh 顶部的 -e 把 tcpkali 的非零退出当致命错误
    set +e
    timeout "${hard_timeout}s" tcpkali \
        --workers "$TCPKALI_WORKERS" \
        -c "$conns" \
        --connect-rate "$cr" \
        --duration "$dur" \
        --latency-marker '@' \
        -m "$msg" \
        --verbose 2 \
        "$@" \
        "$HOST:$PORT" 2>&1
    local ec=$?
    set -e
    if [ "$ec" -eq 124 ]; then
        echo "[tcpkali] HARD TIMEOUT after ${hard_timeout}s (likely TIME-WAIT exhaustion)"
    elif [ "$ec" -ne 0 ]; then
        echo "[tcpkali] exit=$ec (likely TIME-WAIT port exhaustion or connect timeout)"
    fi
    return 0
}

# 从 tcpkali raw 输出抽指标。stdin = raw 文本
# 输出格式（一行 KEY=val 空格分隔）：
#   qps=N bw_mbps=N p95_ms=N p99_ms=N p995_ms=N bytes=N dur_s=N fails=N
parse_tcpkali() {
    awk -v MSG="$MSG_SIZE" '
        /^Total data sent:.*\(([0-9]+) bytes\)/ {
            match($0, /\(([0-9]+) bytes\)/, m); bytes = m[1]
        }
        /^Test duration:/ {
            for (i=1;i<=NF;i++) if ($i+0>0) { dur=$i; break }
        }
        /^Latency at percentiles:/ {
            # "Latency at percentiles: 0.6/0.8/0.9 (95/99/99.5%)"
            sub(/.*: /, ""); sub(/ \(.*/, "")
            n = split($0, p, "/")
            if (n>=3) { p95=p[1]; p99=p[2]; p995=p[3] }
        }
        /connection_failures/ {
            for (i=1;i<=NF;i++) if ($i+0>=0 && $i ~ /^[0-9]+$/) { fails+=$i; break }
        }
        /Aggregate bandwidth:.*Mbps/ {
            # "Aggregate bandwidth: 1400.065↓, 1400.571↑ Mbps" — 取↓方向
            match($0, /([0-9.]+)↓/, m); if (m[1]!="") bw=m[1]
        }
        END {
            if (dur+0 == 0) dur = 1
            qps = (bytes / MSG) / dur
            printf "qps=%.0f bw_mbps=%s p95_ms=%s p99_ms=%s p995_ms=%s bytes=%d dur_s=%s fails=%d\n",
                   qps, bw+0, p95+0, p99+0, p995+0, bytes+0, dur+0, fails+0
        }
    '
}

# 从 KV 行抽某个键。参数：kv_line key
kv_get() {
    echo "$1" | tr ' ' '\n' | awk -F= -v k="$2" '$1==k{print $2}'
}

# 中位数。stdin = 数字（每行一个）
median() {
    sort -g | awk '{a[NR]=$0} END {if(NR==0){print 0} else {print a[int((NR+1)/2)]}}'
}

# 等 TIME-WAIT 数量降到阈值以下。参数：max_wait_s threshold
# 用途：高连接数测试之间，避免端口耗尽。tcp_tw_reuse=1 开启时几乎瞬间返回
wait_for_drain() {
    local max="${1:-90}" thr="${2:-2000}"
    local elapsed=0
    while [ "$elapsed" -lt "$max" ]; do
        local n
        n=$(ss -tan 2>/dev/null | awk '/TIME-WAIT/' | wc -l)
        if [ "$n" -le "$thr" ]; then return 0; fi
        printf "  [drain] TIME-WAIT=%d > %d, waiting...\n" "$n" "$thr" >&2
        sleep 10
        elapsed=$(( elapsed + 10 ))
    done
    return 1
}

# 追加一行 CSV。第一次写时写表头。
# 参数：csv_path header_line data_line
csv_append() {
    local path="$1" header="$2" data="$3"
    [ -f "$path" ] || echo "$header" > "$path"
    echo "$data" >> "$path"
}

# 打印当前模式
print_mode_banner() {
    if is_local; then
        echo "[mode] LOCAL  ($HOST:$PORT, server managed by script)"
    else
        echo "[mode] REMOTE ($HOST:$PORT, server must be running on remote)"
    fi
    echo "[out]  $OUT_DIR"
    echo "[cfg]  duration=$DURATION rounds=$ROUNDS msg_size=${MSG_SIZE}B tcpkali_workers=$TCPKALI_WORKERS"
}

require_tools
