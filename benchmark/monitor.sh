#!/usr/bin/env bash
# =============================================================================
# 后台采样工具：在另一窗口跑 ./monitor.sh <pid>，输出每秒 CPU/MEM/SCHED
# =============================================================================
# 用法：
#   ./build/example/echo_server_coro 18002 4 &
#   ./benchmark/monitor.sh $!
#   (另一窗口跑 ./benchmark/run_qps.sh)
#   Ctrl-C 停采样
# =============================================================================

set -e
PID="${1:-}"
if [ -z "$PID" ]; then
    echo "Usage: $0 <server_pid>"
    exit 1
fi
if ! kill -0 "$PID" 2>/dev/null; then
    echo "no such pid: $PID"
    exit 1
fi

printf "%-8s %-6s %-6s %-10s %-10s %-10s %-8s\n" \
    "time" "cpu%" "mem%" "rss_kB" "ctx_sw/s" "voluntary" "threads"

last_ctx=0
last_vol=0
while kill -0 "$PID" 2>/dev/null; do
    now=$(date +%H:%M:%S)
    line=$(top -b -n 1 -p "$PID" 2>/dev/null | tail -1)
    cpu=$(awk '{print $9}' <<<"$line")
    mem=$(awk '{print $10}' <<<"$line")
    rss=$(awk '/^VmRSS:/{print $2}' /proc/$PID/status 2>/dev/null)
    th=$(awk '/^Threads:/{print $2}' /proc/$PID/status 2>/dev/null)
    ctx=$(awk '/^nonvoluntary_ctxt_switches:/{print $2}' /proc/$PID/status 2>/dev/null)
    vol=$(awk '/^voluntary_ctxt_switches:/{print $2}' /proc/$PID/status 2>/dev/null)
    d_ctx=$((ctx - last_ctx)); last_ctx="$ctx"
    d_vol=$((vol - last_vol)); last_vol="$vol"
    printf "%-8s %-6s %-6s %-10s %-10s %-10s %-8s\n" \
        "$now" "$cpu" "$mem" "$rss" "$d_ctx" "$d_vol" "$th"
    sleep 1
done
