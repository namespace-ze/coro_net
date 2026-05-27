#!/usr/bin/env bash
# =============================================================================
# 测试 B：延迟分布（闭环 vs 开环）
# =============================================================================
# 固定 conn=100 / msg=64B / workers=4
# 跑两种模式：
#   closed: tcpkali 默认（无 message-rate，流式打满，含 pipelining）
#   open  : --message-rate 限速，每连接固定速率 → 暴露 Coordinated Omission 修正
# 每模式跑 $ROUNDS 轮
# =============================================================================
source "$(dirname "$0")/lib.sh"

WORKERS="${WORKERS:-4}"
CONN="${CONN:-100}"
OPEN_LOOP_RATE="${OPEN_LOOP_RATE:-1000}"   # 每连接 msg/s（开环）

print_mode_banner
echo "[test] B: latency  workers=$WORKERS conn=$CONN open_loop_rate=${OPEN_LOOP_RATE}/conn/s"

csv="$OUT_DIR/latency.csv"
raw_log="$OUT_DIR/latency.log"
header="mode,round,duration_s,qps,bw_mbps,p95_ms,p99_ms,p995_ms,fails"

{
    echo "== Test B: latency distribution =="
    echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "workers=$WORKERS conn=$CONN msg=${MSG_SIZE}B duration=$DURATION rounds=$ROUNDS"
    echo "open_loop_rate=$OPEN_LOOP_RATE per conn/s"
} > "$raw_log"

pid=$(start_server "$WORKERS") || exit 1
sleep 1

run_mode() {
    local mode="$1"; shift
    for r in $(seq 1 "$ROUNDS"); do
        echo "-> mode=$mode round=$r"
        echo "" >> "$raw_log"
        echo "## mode=$mode round=$r" >> "$raw_log"
        raw=$(run_tcpkali "$CONN" "$DURATION" "$@")
        echo "$raw" >> "$raw_log"
        kv=$(echo "$raw" | parse_tcpkali)
        csv_append "$csv" "$header" \
          "$mode,$r,$(kv_get "$kv" dur_s),$(kv_get "$kv" qps),$(kv_get "$kv" bw_mbps),$(kv_get "$kv" p95_ms),$(kv_get "$kv" p99_ms),$(kv_get "$kv" p995_ms),$(kv_get "$kv" fails)"
    done
}

run_mode "closed"
run_mode "open" --message-rate "$OPEN_LOOP_RATE"

stop_server "$pid"

echo ""
echo "== summary (median over $ROUNDS rounds) =="
printf "%-8s %-12s %-10s %-10s %-10s\n" "mode" "qps" "p95_ms" "p99_ms" "p995_ms"
echo "----------------------------------------------------------"
for m in closed open; do
    mq=$(awk -F, -v m="$m" '$1==m{print $4}' "$csv" | median)
    m95=$(awk -F, -v m="$m" '$1==m{print $6}' "$csv" | median)
    m99=$(awk -F, -v m="$m" '$1==m{print $7}' "$csv" | median)
    m995=$(awk -F, -v m="$m" '$1==m{print $8}' "$csv" | median)
    printf "%-8s %-12s %-10s %-10s %-10s\n" "$m" "$mq" "$m95" "$m99" "$m995"
done
echo ""
echo "csv : $csv"
echo "raw : $raw_log"
