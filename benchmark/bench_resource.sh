#!/usr/bin/env bash
# =============================================================================
# 测试 F：资源效率探针（每请求 syscall 数 / 每连接内存）—— 在 SERVER 机器上跑
# =============================================================================
# 回答两个 io_uring 网络库最该秀的问题：
#   1. 每请求几个 syscall？io_uring 的卖点就是把 N 个 read/write 批成一次
#      io_uring_enter。strace -c 的直方图里若几乎只见 io_uring_enter，即坐实。
#   2. 每条长连接占多少内存？= (负载态 RSS − 空载 RSS) / 连接数。
#
# 用法（SERVER 机器，server 已在跑）：
#   ./benchmark/bench_resource.sh <server_pid> <seconds> [conn_count]
#
# 典型流程（双机）：
#   SERVER:  ./benchmark/server_runner.sh 12        # 另一窗口，记下 pid
#            ./benchmark/bench_resource.sh <pid> 15 1000
#   CLIENT:  （在 bench_resource 采样的 15s 内）跑一个稳定负载，例如：
#            SERVER_HOST=<ip> CONN=1000 DURATION=20s OPEN_LOOP_RATE=5000 \
#            bash benchmark/run_latency.sh   # 或任意能维持连接的压测
#
# syscalls/请求 = strace 总 syscall ÷（client 端该窗口实际处理的 msg 数，
#                 从 client 的 tcpkali 输出 qps×秒 读）。本脚本只给分子。
#
# 注意：strace 需要 ptrace 权限。若报 "Operation not permitted"：
#   - 用同一用户跑（server 与本脚本同 user），或
#   - sudo ./benchmark/bench_resource.sh ...，或
#   - sudo sysctl -w kernel.yama.ptrace_scope=0（临时放开）
# strace attach 会让 server 变慢（仅采样期间），别拿采样期间的 QPS 当性能数。
# =============================================================================
set -eo pipefail

PID="${1:-}"
SECONDS_N="${2:-15}"
CONN_COUNT="${3:-}"

if [ -z "$PID" ]; then
    echo "Usage: $0 <server_pid> <seconds> [conn_count]" >&2
    exit 1
fi
if ! kill -0 "$PID" 2>/dev/null; then
    echo "ERROR: no such pid: $PID" >&2
    exit 1
fi
command -v strace >/dev/null 2>&1 || {
    echo "ERROR: strace not found. Install: apt install strace" >&2
    exit 1
}

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${OUT_DIR:-$BENCH_DIR/results/resource/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT_DIR"
strace_log="$OUT_DIR/strace.txt"
report="$OUT_DIR/resource.txt"

rss_of()     { awk '/^VmRSS:/{print $2}' "/proc/$1/status" 2>/dev/null; }
threads_of() { awk '/^Threads:/{print $2}' "/proc/$1/status" 2>/dev/null; }
fds_of()     { ls "/proc/$1/fd" 2>/dev/null | wc -l; }

echo "== Test F: resource probe =="
echo "pid=$PID sample=${SECONDS_N}s conn_count=${CONN_COUNT:-<not given>}"
echo ""

# ---- 1. 空载快照 ----
rss_idle=$(rss_of "$PID");  th_idle=$(threads_of "$PID");  fd_idle=$(fds_of "$PID")
echo "[idle]   RSS=${rss_idle} kB  threads=${th_idle}  fds=${fd_idle}"
echo ">>> 现在去 CLIENT 端启动稳定负载（${SECONDS_N}s 内保持连接），本脚本开始 strace 采样..."

# ---- 2. strace -c 采样 N 秒（SIGINT 让 strace 打印 -c 汇总）----
set +e
timeout -s INT "${SECONDS_N}s" strace -f -c -p "$PID" 2>"$strace_log"
set -e

# ---- 3. 负载态快照（趁 client 负载还在）----
rss_load=$(rss_of "$PID");  th_load=$(threads_of "$PID");  fd_load=$(fds_of "$PID")
echo ""
echo "[loaded] RSS=${rss_load} kB  threads=${th_load}  fds=${fd_load}"

# ---- 4. 计算 + 报告 ----
rss_delta=$(( rss_load - rss_idle ))
{
    echo "== Test F: resource probe report =="
    echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "pid=$PID sample=${SECONDS_N}s conn_count=${CONN_COUNT:-<not given>}"
    echo ""
    echo "-- memory --"
    echo "idle   RSS = ${rss_idle} kB  (threads=${th_idle}, fds=${fd_idle})"
    echo "loaded RSS = ${rss_load} kB  (threads=${th_load}, fds=${fd_load})"
    echo "delta  RSS = ${rss_delta} kB"
    if [ -n "$CONN_COUNT" ] && [ "$CONN_COUNT" -gt 0 ] 2>/dev/null; then
        per_conn=$(awk -v d="$rss_delta" -v c="$CONN_COUNT" 'BEGIN{printf("%.2f", d*1024.0/c)}')
        echo "per-connection memory ≈ ${per_conn} bytes  (= delta_RSS / ${CONN_COUNT} conn)"
    else
        echo "per-connection memory: 传入 conn_count 才能算（第 3 个参数）"
    fi
    echo ""
    echo "-- syscall histogram (strace -f -c, ${SECONDS_N}s window) --"
    echo "解读：io_uring 库应几乎只见 io_uring_enter；read/write/recvfrom/sendto 计数"
    echo "      若同样高，说明没走 io_uring 批处理路径，需排查。"
    echo "syscalls/请求 = 下表 total ÷ client 该窗口 msg 数（从 client tcpkali qps×秒 读）"
    echo ""
    cat "$strace_log"
} | tee "$report"

echo ""
echo "report : $report"
echo "strace : $strace_log"
