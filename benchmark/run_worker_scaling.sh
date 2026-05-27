#!/usr/bin/env bash
# =============================================================================
# 测试 D：Worker 扩展性
# =============================================================================
# 固定 conn=100 / msg=64B，扫 workers={1,2,4,8}
# 每个 worker 数下脚本重启 server；远程模式则提示用户手动重启
# =============================================================================
source "$(dirname "$0")/lib.sh"

CONN="${CONN:-100}"
WORKERS_LIST="${WORKERS_LIST:-1 2 4 8}"

print_mode_banner
echo "[test] D: worker scaling  conn=$CONN workers={$WORKERS_LIST}"

csv="$OUT_DIR/workers.csv"
raw_log="$OUT_DIR/workers.log"
header="workers,round,duration_s,qps,bw_mbps,p95_ms,p99_ms,p995_ms,fails"

{
    echo "== Test D: worker scaling =="
    echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "conn=$CONN msg=${MSG_SIZE}B duration=$DURATION rounds=$ROUNDS"
} > "$raw_log"

for w in $WORKERS_LIST; do
    if ! is_local; then
        echo ""
        echo ">>> [REMOTE] Restart server on $HOST with workers=$w, then press Enter..."
        echo "    Example: ./benchmark/server_runner.sh $w $PORT"
        read -r _
    fi

    pid=$(start_server "$w") || exit 1
    sleep 1

    for r in $(seq 1 "$ROUNDS"); do
        echo "-> workers=$w round=$r"
        echo "" >> "$raw_log"
        echo "## workers=$w round=$r" >> "$raw_log"
        raw=$(run_tcpkali "$CONN" "$DURATION")
        echo "$raw" >> "$raw_log"
        kv=$(echo "$raw" | parse_tcpkali)
        csv_append "$csv" "$header" \
          "$w,$r,$(kv_get "$kv" dur_s),$(kv_get "$kv" qps),$(kv_get "$kv" bw_mbps),$(kv_get "$kv" p95_ms),$(kv_get "$kv" p99_ms),$(kv_get "$kv" p995_ms),$(kv_get "$kv" fails)"
    done

    stop_server "$pid"
    sleep 1
done

# 扩展比（相对 1 worker 基线）
echo ""
echo "== summary (median over $ROUNDS rounds) =="
printf "%-8s %-12s %-10s %-10s %-12s\n" "workers" "qps" "p95_ms" "p99_ms" "scale"
echo "----------------------------------------------------------"
base=""
for w in $WORKERS_LIST; do
    mq=$(awk -F, -v w="$w" '$1==w{print $4}' "$csv" | median)
    m95=$(awk -F, -v w="$w" '$1==w{print $6}' "$csv" | median)
    m99=$(awk -F, -v w="$w" '$1==w{print $7}' "$csv" | median)
    if [ -z "$base" ]; then base="$mq"; fi
    scale=$(awk -v q="$mq" -v b="$base" -v w="$w" \
        'BEGIN { if(b+0>0) printf("%.2fx (%.0f%%)", q/b, q/(b*w)*100); else print "-" }')
    printf "%-8s %-12s %-10s %-10s %-12s\n" "$w" "$mq" "$m95" "$m99" "$scale"
done
echo ""
echo "csv : $csv"
echo "raw : $raw_log"
