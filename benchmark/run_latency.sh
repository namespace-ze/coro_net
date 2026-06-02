#!/usr/bin/env bash
# =============================================================================
# 测试 B：延迟分布（Coordinated Omission 演示 + 开环负载扫描）
# =============================================================================
# 固定 conn=100 / msg=64B / workers=12，分两段：
#
#   B1  CO 演示（保留原对比）
#       closed: tcpkali 默认（无 message-rate，pipelined 流式打满 → 饱和）
#       open  : --message-rate 限速（开环，offered load = conn × rate）
#       两者 p99 差出数百倍，直观演示 Coordinated Omission（详见 BENCHMARK.md §一.4）
#
#   B2  开环负载扫描（核心新增）
#       固定 conn，扫一组 per-conn rate → 总 offered load 递增，
#       记每档 p99 → 画 "offered load vs p99" 曲线，找 SLO 拐点
#       （p99 离开地板那一点，才是 "SLO 约束下的可持续吞吐"）
#
# 想测 "连接数 × 负载" 二维矩阵：改 CONN 环境变量重跑本脚本即可，
# 不在脚本里硬扫二维（耗时太长）。
# =============================================================================
source "$(dirname "$0")/lib.sh"

WORKERS="${WORKERS:-12}"
CONN="${CONN:-100}"
# B1 CO 演示用的参考开环速率（每连接 msg/s）
OPEN_LOOP_RATE="${OPEN_LOOP_RATE:-5000}"
# B2 负载扫描档位（每连接 msg/s）。× CONN = 总 offered load。
# 默认 2000..50000 → conn=100 时为 200K / 500K / 1M / 2M / 5M offered load。
OPEN_LOOP_RATE_LIST="${OPEN_LOOP_RATE_LIST:-2000 5000 10000 20000 50000}"

print_mode_banner
echo "[test] B: latency  workers=$WORKERS conn=$CONN"
echo "       B1 CO demo (closed vs open@${OPEN_LOOP_RATE}/conn)"
echo "       B2 load sweep rates/conn={$OPEN_LOOP_RATE_LIST}"

csv="$OUT_DIR/latency.csv"
raw_log="$OUT_DIR/latency.log"
# offered_load = conn × rate_per_conn（closed 模式无固定 rate，填 0=N/A）
header="phase,mode,rate_per_conn,offered_load,round,duration_s,qps,bw_mbps,p95_ms,p99_ms,p995_ms,fails"

{
    echo "== Test B: latency (CO demo + open-loop load sweep) =="
    echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "workers=$WORKERS conn=$CONN msg=${MSG_SIZE}B duration=$DURATION rounds=$ROUNDS"
    echo "B1 co_demo open_rate=$OPEN_LOOP_RATE/conn/s"
    echo "B2 sweep rates/conn={$OPEN_LOOP_RATE_LIST}"
} > "$raw_log"

pid=$(start_server "$WORKERS") || exit 1
sleep 1

# 跑一档并落 CSV。
# 参数：phase mode rate_per_conn [tcpkali extra args...]
run_point() {
    local phase="$1" mode="$2" rate="$3"; shift 3
    local offered=$(( CONN * rate ))   # rate=0（closed）时 offered=0 表示 N/A
    for r in $(seq 1 "$ROUNDS"); do
        echo "-> $phase mode=$mode rate=$rate/conn (offered=$offered) round=$r"
        echo "" >> "$raw_log"
        echo "## $phase mode=$mode rate=$rate offered=$offered round=$r" >> "$raw_log"
        local raw kv
        raw=$(run_tcpkali "$CONN" "$DURATION" "$@")
        echo "$raw" >> "$raw_log"
        kv=$(echo "$raw" | parse_tcpkali)
        csv_append "$csv" "$header" \
          "$phase,$mode,$rate,$offered,$r,$(kv_get "$kv" dur_s),$(kv_get "$kv" qps),$(kv_get "$kv" bw_mbps),$(kv_get "$kv" p95_ms),$(kv_get "$kv" p99_ms),$(kv_get "$kv" p995_ms),$(kv_get "$kv" fails)"
    done
}

# ---- B1: CO 演示（closed 饱和 vs open 参考负载）----
echo ""
echo "== B1: Coordinated Omission demo =="
run_point "B1" "closed" 0
run_point "B1" "open"   "$OPEN_LOOP_RATE" --message-rate "$OPEN_LOOP_RATE"

# ---- B2: 开环负载扫描 ----
echo ""
echo "== B2: open-loop load sweep (find SLO knee) =="
for rate in $OPEN_LOOP_RATE_LIST; do
    run_point "B2" "open" "$rate" --message-rate "$rate"
done

stop_server "$pid"

# ---- 汇总 ----
echo ""
echo "== B1 summary (median over $ROUNDS rounds) =="
printf "%-8s %-12s %-10s %-10s %-10s\n" "mode" "qps" "p95_ms" "p99_ms" "p995_ms"
echo "----------------------------------------------------------"
for m in closed open; do
    mq=$(awk -F, -v m="$m" '$1=="B1"&&$2==m{print $7}' "$csv" | median)
    m95=$(awk -F, -v m="$m" '$1=="B1"&&$2==m{print $9}' "$csv" | median)
    m99=$(awk -F, -v m="$m" '$1=="B1"&&$2==m{print $10}' "$csv" | median)
    m995=$(awk -F, -v m="$m" '$1=="B1"&&$2==m{print $11}' "$csv" | median)
    printf "%-8s %-12s %-10s %-10s %-10s\n" "$m" "$mq" "$m95" "$m99" "$m995"
done

echo ""
echo "== B2 summary: offered load vs p99 (median over $ROUNDS rounds) =="
printf "%-14s %-12s %-10s %-10s %-8s\n" "offered_load" "actual_qps" "p95_ms" "p99_ms" "fails"
echo "----------------------------------------------------------"
for rate in $OPEN_LOOP_RATE_LIST; do
    offered=$(( CONN * rate ))
    mq=$(awk -F, -v o="$offered" '$1=="B2"&&$4==o{print $7}' "$csv" | median)
    m95=$(awk -F, -v o="$offered" '$1=="B2"&&$4==o{print $9}' "$csv" | median)
    m99=$(awk -F, -v o="$offered" '$1=="B2"&&$4==o{print $10}' "$csv" | median)
    mf=$(awk -F, -v o="$offered" '$1=="B2"&&$4==o{print $12}' "$csv" | median)
    printf "%-14s %-12s %-10s %-10s %-8s\n" "$offered" "$mq" "$m95" "$m99" "$mf"
done
echo ""
echo "提示：p99 开始明显离开地板的那一档 offered load = SLO 约束下的可持续吞吐拐点。"
echo ""
echo "csv : $csv"
echo "raw : $raw_log"
