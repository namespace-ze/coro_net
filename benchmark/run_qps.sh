#!/usr/bin/env bash
# =============================================================================
# 测试 A：QPS 扫描
# =============================================================================
# 固定 workers=4 / msg=64B，扫 connections={10,100,1000,5000}
# 每档跑 $ROUNDS 轮，输出 CSV + raw log
# =============================================================================
source "$(dirname "$0")/lib.sh"

WORKERS="${WORKERS:-4}"
CONNECTIONS_LIST="${CONNECTIONS_LIST:-10 100 1000 5000}"

print_mode_banner
echo "[test] A: QPS scan  workers=$WORKERS conns={$CONNECTIONS_LIST}"

csv="$OUT_DIR/qps.csv"
raw_log="$OUT_DIR/qps.log"
header="conns,round,duration_s,qps,bw_mbps,p95_ms,p99_ms,p995_ms,fails"

{
    echo "== Test A: QPS scan =="
    echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "workers=$WORKERS msg=${MSG_SIZE}B duration=$DURATION rounds=$ROUNDS"
} > "$raw_log"

# 仅本地模式：脚本统一起一次 server，扫完所有连接数后停
pid=$(start_server "$WORKERS") || exit 1
sleep 1

for c in $CONNECTIONS_LIST; do
    if [ "$c" -ge 500 ]; then wait_for_drain 60 2000 || true; fi
    for r in $(seq 1 "$ROUNDS"); do
        echo "-> conns=$c round=$r"
        echo "" >> "$raw_log"
        echo "## conns=$c round=$r" >> "$raw_log"
        raw=$(run_tcpkali "$c" "$DURATION")
        echo "$raw" >> "$raw_log"
        kv=$(echo "$raw" | parse_tcpkali)
        csv_append "$csv" "$header" \
          "$c,$r,$(kv_get "$kv" dur_s),$(kv_get "$kv" qps),$(kv_get "$kv" bw_mbps),$(kv_get "$kv" p95_ms),$(kv_get "$kv" p99_ms),$(kv_get "$kv" p995_ms),$(kv_get "$kv" fails)"
    done
done

stop_server "$pid"

# 打印汇总表（每档取 QPS / p99 中位）
echo ""
echo "== summary (median over $ROUNDS rounds) =="
printf "%-8s %-12s %-10s %-10s %-10s\n" "conns" "qps" "p95_ms" "p99_ms" "p995_ms"
echo "----------------------------------------------------------"
for c in $CONNECTIONS_LIST; do
    mq=$(awk -F, -v c="$c" '$1==c{print $4}' "$csv" | median)
    m95=$(awk -F, -v c="$c" '$1==c{print $6}' "$csv" | median)
    m99=$(awk -F, -v c="$c" '$1==c{print $7}' "$csv" | median)
    m995=$(awk -F, -v c="$c" '$1==c{print $8}' "$csv" | median)
    printf "%-8s %-12s %-10s %-10s %-10s\n" "$c" "$mq" "$m95" "$m99" "$m995"
done
echo ""
echo "csv : $csv"
echo "raw : $raw_log"
