#!/usr/bin/env bash
# =============================================================================
# 测试 E：消息大小扫描（吞吐 / 带宽 / 延迟 随消息大小变化）
# =============================================================================
# 固定 workers=12 / conn=1000，扫 msg_size={64,256,1024,4096,16384} 字节
# 每档跑 $ROUNDS 轮，输出 CSV + raw log。
#
# 为什么要扫消息大小（业界标准轴，muduo ping-pong 图同款）：
#   - 小包（64-256B）：吞吐受 syscall / 调度 / per-message 开销主导 → 看 msg/s
#   - 大包（4-16KB）：吞吐受内存拷贝 / 网卡带宽主导 → 看 Gbps，msg/s 反而降
#   两端的瓶颈不同，单一 64B 数字说明不了全貌。
#
# 注意：MSG_SIZE 是逐档覆盖的，所以本脚本自己管 MSG_SIZE，不读外部默认值。
# =============================================================================
source "$(dirname "$0")/lib.sh"

WORKERS="${WORKERS:-12}"
CONN="${CONN:-1000}"
MSG_SIZE_LIST="${MSG_SIZE_LIST:-64 256 1024 4096 16384}"

print_mode_banner
echo "[test] E: message-size scan  workers=$WORKERS conn=$CONN sizes={$MSG_SIZE_LIST}"

csv="$OUT_DIR/msgsize.csv"
raw_log="$OUT_DIR/msgsize.log"
header="msg_size,round,duration_s,qps,bw_mbps,bw_gbps,p95_ms,p99_ms,p995_ms,fails"

{
    echo "== Test E: message-size scan =="
    echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "workers=$WORKERS conn=$CONN sizes={$MSG_SIZE_LIST} duration=$DURATION rounds=$ROUNDS"
} > "$raw_log"

# 起一次 server，扫完所有消息大小后停（消息大小是 client 侧参数，不影响 server）
pid=$(start_server "$WORKERS") || exit 1
sleep 1

for sz in $MSG_SIZE_LIST; do
    export MSG_SIZE="$sz"   # run_tcpkali / make_message 读这个全局
    wait_for_drain 60 2000 || true
    for r in $(seq 1 "$ROUNDS"); do
        echo "-> msg_size=$sz round=$r"
        echo "" >> "$raw_log"
        echo "## msg_size=$sz round=$r" >> "$raw_log"
        raw=$(run_tcpkali "$CONN" "$DURATION")
        echo "$raw" >> "$raw_log"
        kv=$(echo "$raw" | parse_tcpkali)
        bw_mbps=$(kv_get "$kv" bw_mbps)
        bw_gbps=$(awk -v m="$bw_mbps" 'BEGIN{printf("%.2f", m/1000.0)}')
        csv_append "$csv" "$header" \
          "$sz,$r,$(kv_get "$kv" dur_s),$(kv_get "$kv" qps),$bw_mbps,$bw_gbps,$(kv_get "$kv" p95_ms),$(kv_get "$kv" p99_ms),$(kv_get "$kv" p995_ms),$(kv_get "$kv" fails)"
    done
done

stop_server "$pid"

# 汇总（每档取 msg/s / Gbps / p99 中位）
echo ""
echo "== summary (median over $ROUNDS rounds) =="
printf "%-10s %-14s %-10s %-10s %-10s\n" "msg_size" "qps" "bw_gbps" "p99_ms" "fails"
echo "----------------------------------------------------------"
for sz in $MSG_SIZE_LIST; do
    mq=$(awk -F, -v s="$sz" '$1==s{print $4}' "$csv" | median)
    mg=$(awk -F, -v s="$sz" '$1==s{print $6}' "$csv" | median)
    m99=$(awk -F, -v s="$sz" '$1==s{print $8}' "$csv" | median)
    mf=$(awk -F, -v s="$sz" '$1==s{print $10}' "$csv" | median)
    printf "%-10s %-14s %-10s %-10s %-10s\n" "$sz" "$mq" "$mg" "$m99" "$mf"
done
echo ""
echo "csv : $csv"
echo "raw : $raw_log"
