#!/usr/bin/env bash
# =============================================================================
# 测试 C：连接数极限
# =============================================================================
# workers=4 / msg=64B，c 从 1k 递增到失败或 QPS 塌缩（>10% connect failures 就停）
# 注意：本机 ip_local_port_range 决定可用客户端端口（默认约 28K）
# =============================================================================
source "$(dirname "$0")/lib.sh"

WORKERS="${WORKERS:-4}"
CONN_LIST="${CONN_LIST:-1000 2000 5000 10000 20000}"
DURATION="${DURATION:-10s}"  # 大连接数下短时即可观察

print_mode_banner
echo "[test] C: connection limit  workers=$WORKERS conns={$CONN_LIST}"

csv="$OUT_DIR/connlimit.csv"
raw_log="$OUT_DIR/connlimit.log"
header="conns,duration_s,qps,bw_mbps,p95_ms,p99_ms,p995_ms,fails,err_pct"

{
    echo "== Test C: connection limit =="
    echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "workers=$WORKERS msg=${MSG_SIZE}B duration=$DURATION"
    echo "ulimit -n: $(ulimit -n)"
    echo "ip_local_port_range: $(cat /proc/sys/net/ipv4/ip_local_port_range 2>/dev/null || echo n/a)"
    echo "somaxconn: $(cat /proc/sys/net/core/somaxconn 2>/dev/null || echo n/a)"
} > "$raw_log"

DUR_NUM="${DURATION%[smh]}"
for c in $CONN_LIST; do
    wait_for_drain 90 2000 || true
    pid=$(start_server "$WORKERS") || exit 1
    sleep 1

    echo "-> conns=$c"
    echo "" >> "$raw_log"
    echo "## conns=$c" >> "$raw_log"
    raw=$(run_tcpkali "$c" "$DURATION")
    echo "$raw" >> "$raw_log"
    kv=$(echo "$raw" | parse_tcpkali)
    qps=$(kv_get "$kv" qps)
    dur_s=$(kv_get "$kv" dur_s)
    fails=$(kv_get "$kv" fails)
    err_pct=$(awk -v f="$fails" -v c="$c" 'BEGIN{printf("%.1f", (c>0?f*100.0/c:0))}')
    csv_append "$csv" "$header" \
      "$c,$dur_s,$qps,$(kv_get "$kv" bw_mbps),$(kv_get "$kv" p95_ms),$(kv_get "$kv" p99_ms),$(kv_get "$kv" p995_ms),$fails,$err_pct"

    stop_server "$pid"
    sleep 2

    # 三种停止条件：
    #   1) 失败率 > 10%
    #   2) tcpkali 没跑完（dur_s < 50% expected → 连接阶段超时/被 kill）
    if awk -v p="$err_pct" 'BEGIN{exit !(p>10)}'; then
        echo ">>> >10% connect failures at c=$c (fails=$fails), stopping"
        break
    fi
    if awk -v d="$dur_s" -v e="$DUR_NUM" 'BEGIN{exit !(d < e*0.5)}'; then
        echo ">>> tcpkali aborted early at c=$c (dur=${dur_s}s < ${DUR_NUM}s*0.5), likely TIME-WAIT exhaustion. stopping"
        echo "    Hint: enable 'sysctl -w net.ipv4.tcp_tw_reuse=1' to push the limit higher."
        break
    fi
done

echo ""
echo "== summary =="
printf "%-8s %-12s %-10s %-10s %-10s %-10s\n" "conns" "qps" "p95_ms" "p99_ms" "fails" "err_pct"
echo "----------------------------------------------------------"
awk -F, 'NR>1 { printf "%-8s %-12s %-10s %-10s %-10s %-10s\n", $1,$3,$5,$6,$8,$9 }' "$csv"
echo ""
echo "csv : $csv"
echo "raw : $raw_log"
