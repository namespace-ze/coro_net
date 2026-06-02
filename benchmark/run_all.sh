#!/usr/bin/env bash
# =============================================================================
# benchmark/run_all.sh —— 一键跑全部四类测试 + 收集环境信息
# =============================================================================
# 用法：
#   bash benchmark/run_all.sh                           # 本机默认参数
#   DURATION=60s ROUNDS=3 bash benchmark/run_all.sh     # 云上推荐参数
#   HOST=10.0.0.5 bash benchmark/run_all.sh             # 远程 server（需手动起）
#
# 注意：OUT_DIR 在 lib.sh 里默认按时间戳分子目录；这里固定一个 ts 让四类测试共用
# =============================================================================

set -eo pipefail

ts=$(date +%Y%m%d-%H%M%S)
HOST="${HOST:-127.0.0.1}"
case "$HOST" in
    127.0.0.1|localhost|::1) tag="local";;
    *) tag="cloud";;
esac
export OUT_DIR="${OUT_DIR:-$(cd "$(dirname "$0")" && pwd)/results/$tag/$ts}"
mkdir -p "$OUT_DIR"

# ---- 环境快照 ----
env_file="$OUT_DIR/env.txt"
{
    echo "== environment snapshot =="
    echo "timestamp: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host: $HOST  port: ${PORT:-18002}"
    echo "duration: ${DURATION:-10s}  rounds: ${ROUNDS:-1}  msg_size: ${MSG_SIZE:-64}"
    echo ""
    echo "-- uname --"; uname -a
    echo ""
    echo "-- cpu --"; lscpu 2>/dev/null | grep -E "Model name|Socket|Core|CPU\(s\)|MHz" | head -10
    echo ""
    echo "-- memory --"; grep -E "MemTotal|MemAvailable" /proc/meminfo 2>/dev/null
    echo ""
    echo "-- ulimit -n --"; ulimit -n
    echo ""
    echo "-- sysctl --"
    for k in net.core.somaxconn net.core.rmem_max net.core.wmem_max \
             net.core.netdev_max_backlog \
             net.ipv4.tcp_max_syn_backlog net.ipv4.ip_local_port_range \
             net.ipv4.tcp_tw_reuse fs.file-max; do
        printf "%-40s = %s\n" "$k" "$(sysctl -n "$k" 2>/dev/null || echo n/a)"
    done
    # 16 vCPU 上若要把 server 钉核避免与内核软中断抢核，建议：
    #   taskset -c 0-11 ./build/example/echo_server_coro 18002 12
    # （留 12-15 给 NIC IRQ / io_uring helper）。本快照不强制，仅记录建议。
    echo ""
    echo "-- tcpkali --"; tcpkali --version 2>&1 | head -2
    echo ""
    echo "-- coro_net build --"
    ls -la "$(cd "$(dirname "$0")/.." && pwd)/build/example/echo_server_coro" 2>/dev/null || echo "(echo_server_coro not built)"
} > "$env_file"

echo "[run_all] env saved to $env_file"
echo "[run_all] OUT_DIR = $OUT_DIR"
echo ""

# ---- 五类测试（A QPS / B 延迟 / C 连接极限 / D worker 扩展 / E 消息大小）----
# OUT_DIR 已 export，子脚本 source lib.sh 时会复用
# 注：测试 F（资源探针 bench_resource.sh）在 SERVER 机器上手动跑，不进 run_all
#     （run_all 是 client 侧编排，无法 attach 远程 server 进程）。流程见 REPRODUCE.md。
bash "$(dirname "$0")/run_qps.sh"
echo ""
bash "$(dirname "$0")/run_latency.sh"
echo ""
bash "$(dirname "$0")/run_conn_limit.sh"
echo ""
bash "$(dirname "$0")/run_worker_scaling.sh"
echo ""
bash "$(dirname "$0")/run_msg_size.sh"

echo ""
echo "==============================================================="
echo "  All tests done. Results: $OUT_DIR"
echo "  （测试 F 资源探针请在 server 机器上单独跑 bench_resource.sh）"
echo "==============================================================="
ls -la "$OUT_DIR"
