# 压测复现指南（含云上完整流程）

> 一份**够用就走**的复现文档。本机和云机都按这一份做。
> 数据基线、坑、退路都在这里。脚本本身不安装任何工具，所有依赖你自己装。
>
> **赶时间？** 用阿里云两台 8C32G 跑：直接跳到 [§11 阿里云 8C32G × 2 实战 playbook](#11-阿里云-8c32g-2-实战-playbook)，从 Step 0 顺序往下做。

---

## 1. 前置依赖（用户自装，脚本不管）

**两个二进制必须在 PATH 中**：

| 工具 | 用途 | 安装 |
|---|---|---|
| `tcpkali` (≥ v0.4) | 唯一负载发生器，QPS + 延迟分位 | Debian/Ubuntu 较新版：`apt install tcpkali`；其它发行版从源码编译：https://github.com/satori-com/tcpkali |
| `g++-13` + `liburing-dev` + `cmake` | 编 coro_net | `apt install build-essential g++-13 cmake liburing-dev pkg-config` |

脚本启动时会 `command -v tcpkali`，缺则报错退出——不会自动安装。

> 为什么不写 `apt install`？部分发行版（RHEL / CentOS / Rocky）apt 没 tcpkali，要从源码编。文档不替你做这个判断。

---

## 2. 编译 coro_net

```bash
cd <repo-root>
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13
cmake --build build -j
```

产出 `build/example/echo_server_coro`。

---

## 3. 系统调优（强烈推荐，不调 ≥1000 连接测试会失败）

**所有高连接数测试都依赖这几项**。云机首次开机后跑：

```bash
# 1. 文件描述符上限
ulimit -n 1000000                     # 当前 shell
echo '* soft nofile 1000000' | sudo tee -a /etc/security/limits.conf
echo '* hard nofile 1000000' | sudo tee -a /etc/security/limits.conf

# 2. 内核 TCP 参数
sudo sysctl -w net.core.somaxconn=65535 \
              net.ipv4.tcp_max_syn_backlog=65535 \
              net.ipv4.ip_local_port_range="10000 65535" \
              net.core.rmem_max=16777216 \
              net.core.wmem_max=16777216 \
              net.ipv4.tcp_tw_reuse=1 \
              fs.file-max=2000000

# 3. 验证
ulimit -n         # 应为 1000000
sysctl net.ipv4.tcp_tw_reuse   # 应为 1
```

> **重点是 `tcp_tw_reuse=1`**。默认值（`2` 或 `0`）会让 ≥1000 连接的反复压测因端口耗尽失败。本机若是 WSL2 / 容器，部分 sysctl 可能没权限写——这是 **WSL2 上 1000+ conn 测试不可靠** 的根因，cloud Linux native 没有这个限制。

---

## 4. 一键全跑

```bash
# 云上推荐参数（30~60s / 测试，3 轮取中位）
DURATION=60s ROUNDS=3 bash benchmark/run_all.sh

# 本机快验（10s / 测试，1 轮）
DURATION=10s ROUNDS=1 bash benchmark/run_all.sh
```

输出落到 `benchmark/results/<local|cloud>/<YYYYMMDD-HHMMSS>/`，每次跑都是独立目录，不会覆盖历史。

---

## 5. 单类测试

四个 `run_*.sh` 各跑一类，参数全用环境变量：

```bash
# 测试 A：QPS 扫描（不同并发连接数下的吞吐 + p99）
CONNECTIONS_LIST="10 100 1000 5000" \
WORKERS=4 \
DURATION=30s ROUNDS=3 \
bash benchmark/run_qps.sh

# 测试 B：延迟分布（闭环 vs 开环对比）
CONN=100 \
OPEN_LOOP_RATE=2000 \
DURATION=60s ROUNDS=3 \
bash benchmark/run_latency.sh

# 测试 C：连接数极限（错误率 >10% 或 tcpkali 超时即停）
CONN_LIST="1000 2000 5000 10000 20000" \
DURATION=15s \
bash benchmark/run_conn_limit.sh

# 测试 D：Worker 扩展性
WORKERS_LIST="1 2 4 8" \
CONN=100 \
DURATION=30s ROUNDS=3 \
bash benchmark/run_worker_scaling.sh
```

### 通用环境变量

| 变量 | 默认 | 含义 |
|---|---|---|
| `HOST` | `127.0.0.1` | 服务端 IP；远程 IP 时脚本不起 server，需自己起好 |
| `PORT` | `18002` | 服务端端口 |
| `DURATION` | `10s` | 单次 tcpkali 时长 |
| `ROUNDS` | `1` | 每数据点重复轮数，取中位 |
| `MSG_SIZE` | `64` | 消息字节数（含 latency marker `@`） |
| `OUT_DIR` | `benchmark/results/<local|cloud>/<ts>` | 输出目录 |
| `TCPKALI_WORKERS` | `$(nproc)` | tcpkali 自身线程数，默认 = client 核数。**不要降**——4 worker 在 ≥10K conn + `--latency-marker` 下 client CPU 顶不住，建连卡住 |
| `CONNECT_RATE` | `1000` | tcpkali 建连接速率（SYN/s）。**云上保持默认**，否则会被云厂商 SYN flood 防护拦截（见 §8）。裸金属可拉到 5000-10000。 |

---

## 6. 双机模式（云上真实压测）

同区域两台云机：一台跑 server（独占机器），一台跑 client（独占机器）。

```bash
#### server 机器 ####
./benchmark/server_runner.sh 4          # 4 worker，前台跑

#### client 机器 ####
SERVER_HOST=10.0.0.5 \                  # ← server 内网 IP
DURATION=60s ROUNDS=3 \
bash benchmark/run_all.sh
```

注意：
- `SERVER_HOST` 不是 `127.0.0.1` 时，脚本**自动切到 REMOTE 模式**——不起 server，假定远程已起好。
- 测试 D（worker 扩展）每个 worker 数都要重启 server。脚本会**逐次提示你按 Enter**，对照另一窗口的 `server_runner.sh <new_workers>` 即可。
- 安全组只开 client → server 内网，别全开 0.0.0.0/0。

---

## 7. 输出说明

每次跑产出：

```
results/<local|cloud>/<ts>/
├── env.txt                # 环境快照（CPU/内存/sysctl/工具版本）
├── qps.csv                # 测试 A 数据
├── qps.log                # 测试 A 原始 tcpkali 输出
├── latency.csv / .log
├── connlimit.csv / .log
├── workers.csv / .log
└── server-w<N>-<ts>.log   # echo_server stderr
```

### CSV schema

| 文件 | 列 |
|---|---|
| `qps.csv` | `conns,round,duration_s,qps,bw_mbps,p95_ms,p99_ms,p995_ms,fails` |
| `latency.csv` | `mode,round,duration_s,qps,bw_mbps,p95_ms,p99_ms,p995_ms,fails` |
| `connlimit.csv` | `conns,duration_s,qps,bw_mbps,p95_ms,p99_ms,p995_ms,fails,err_pct` |
| `workers.csv` | `workers,round,duration_s,qps,bw_mbps,p95_ms,p99_ms,p995_ms,fails` |

字段语义：
- `qps`：消息吞吐（tcpkali 默认是流式 + marker 追踪，不是严格 RR 闭环；闭环数字会显著低于这里）
- `bw_mbps`：聚合下行带宽（Mbps）
- `p95_ms / p99_ms / p995_ms`：tcpkali 0.4 内建百分位（毫秒）
- `fails`：tcpkali `connection_failures` 计数
- `duration_s`：tcpkali 实际跑时长；**显著小于 `DURATION` 表示该轮失败**（如 TIME-WAIT 端口耗尽，tcpkali 没跑完）

---

## 8. 排错

### `[tcpkali] HARD TIMEOUT after Ns`
脚本兜底超时（`--duration + 30s`）触发。常见原因：
- 没设 `net.ipv4.tcp_tw_reuse=1`，TIME-WAIT 端口耗尽
- 连接数远超 `ip_local_port_range` 可用范围

→ 跑 §3 的 sysctl。

### `Could not create N connections in allotted time`
tcpkali 在 `--duration` 内来不及建够连接。原因同上 + `--connect-rate` 默认 100/s 太慢。脚本已经自动按 `conns * 10` 放大 connect-rate。

### `csv` 里某行 `duration_s` 是 `1`（默认 fallback 值）
tcpkali 这一轮失败，没产生有效输出。看同名 `.log` 找 `[tcpkali] exit=` 行确认原因。

### echo_server 启动失败
检查 `build/example/echo_server_coro` 是否 Release 编译（Debug 性能差一半），`ss -tln | grep 18002` 看端口是否被旧实例占着。

### REMOTE 模式下 `cannot reach <host>:<port>`
- server 机器上 `./benchmark/server_runner.sh 4` 没起，或起在不同端口
- 云厂安全组没放行 client IP 到 server 18002

### 云厂商 SYN flood 拦截（≥5000 conn 静默 0 QPS）
**症状**：100 / 1000 conn 正常出数，5000 / 10000 conn 三轮全是 `qps=0` + `duration_s=1`，但 server 端没崩没报错。
**原因**：阿里云 / AWS / GCP 等云厂内网 SLB / DDoS 防护对**短时间高频 SYN** 做拦截。tcpkali 默认 `--connect-rate=100` 不会触发，但脚本旧版按 `conns*10` 自动放宽（c=5000 时是 50000/s SYN）会被云防火墙静默丢包，于是 tcpkali 建不够连接，提早退出。
**缓解**：lib.sh 现已固定默认 `CONNECT_RATE=1000`（云上安全值）。**云上不要覆盖**。如需更高，先在裸金属或私有内网验证云厂规则。
**真实案例**：阿里云 8C32G × 2，c=5000 默认 cr=50000 全失败；改 cr=1000 后 `21.3 Gbps / 60s 全跑完`、c=10000 → `16.3 Gbps`。

### c=10000 卡在 ramp-up 不报错（log 里没 `Ramped up to N connections.`）
**症状**：raw log 显示 tcpkali 启动后只有 `Destination: [...]:18002` 一行，没看到 `Ramped up to ...`，最终被 HARD TIMEOUT 杀。c=5000 正常但 c=10000 卡。
**原因**：tcpkali 默认 4 worker 时，每个 worker 要管 2500 个 conn + `--latency-marker` 逐字节扫描，client CPU 顶不住，建连阶段就跟不上。
**缓解**：lib.sh 现已把 `TCPKALI_WORKERS` 默认改成 `$(nproc)`。8 核 client 上自动是 8 worker，c=10000 顺利 ramp。
**真实案例**：阿里云 8C32G client，TCPKALI_WORKERS=4 c=10000 完全卡死；改 TCPKALI_WORKERS=8 后 32.9M msg/s 跑完 60s。

### `[tcpkali] HARD TIMEOUT` 在 c≥5000 / Ramped up 之后才打死
**症状**：raw log 显示 `Ramped up to N connections.` 成功了，60s `--duration` 也到了，但 tcpkali 不退出，被外层 `timeout` 杀。
**原因**：高 conn + 多 worker 下若加了 `--verbose 2`，tcpkali 每秒每 worker dump 一大坨 stats，shell `$(...)` 子进程的 stdout pipe buffer (~64KB) 会被堵满 → tcpkali 在 write() 系统调用阻塞 → 测试结束后停不下来。
**缓解**：lib.sh 已**不再用 `--verbose 2`**。最终 `Latency at percentiles` 那行就够 parse 所有指标，不需要 verbose。
**别加回来**：以后想拿 per-worker stats 来 debug 时，用 `tcpkali ... > raw.log 2>&1` 直接写文件（不通过 `$()`），不要再放回 lib.sh 默认参数。

---

## 9. 预期数据范围（参考）

### 本机 WSL2 + Intel i5 笔记本（保守下界）

仅作脚本流程验证。**真数据看云机**：

```
qps     conns=10:     ~8M  msg/s (pipelined, 不是 RR)
qps     conns=100:    ~6M  msg/s
latency closed-loop:  p99 ~50-100ms (pipeline 堆积)
latency open-loop:    p99 ~10ms (限速后真实延迟)
```

WSL2 上 ≥1000 连接的测试**通常失败**（tcp_tw_reuse 在 WSL 上行为受限）。

### 云上双机参考

| 指标 (4 worker / 64B msg) | 8C32G × 2 阿里云 (g7/c7.2xlarge) | 16 vCPU × 2 c7i.4xlarge |
|---|---|---|
| 峰值 msg/s @ 1K conn (pipelined) | 500K - 1.5M | 1M - 3M |
| 开环 p99 @ 5K rate | 1 - 3 ms | 0.5 - 1.5 ms |
| 连接极限（0 错误） | ~50K | 100K+ |
| 1→4 worker 扩展比 | 3.0 - 3.6x (75 - 90%) | 3.0 - 3.6x |
| 4→8 worker | 收益递减（CPU 见顶） | 收益递减（CPU 见顶） |

实际数字依赖实例族（g7 vs c7 vs c8i）、是否独占物理核、内核版本与 NIC 队列数。低于这个范围 → 排查 §3 sysctl、安全组、iperf3 网络底数；显著高于 → 检查是否误开了 loopback / 同 host。

### 旧手写客户端基线（闭环 RR 模型，仅供回归对照）

```
QPS @ 100 conn: 291K (closed-loop request-response)
p99: 0.88ms
20K 长连接 0 错误
```

新工具 QPS 数字直接对比这个没意义——tcpkali 是**流式吞吐**，不是 RR。`run_latency.sh` 的 `open` 模式（开环限速）才是可比的延迟数据。

---

## 10. 与旧手写客户端的差异

| 维度 | 旧 echo_client.cc | 新 tcpkali |
|---|---|---|
| 实现 | 250 行 C++ epoll | 第三方 C 程序 |
| 模型 | 严格闭环 RR（发一收一） | 流式 + marker 追踪 |
| 延迟分位 | p50/p99/p999 | p95/p99/p99.5（tcpkali 0.4 唯一支持） |
| 开环支持 | 无 | `--message-rate`（修正 CO） |
| 可信度 | 自写，需评审者读代码 | 行业通用工具，无需解释 |

旧客户端已归档到 `benchmark/legacy/echo_client.cc`，BENCHMARK.md §1.4 仍引用它讲 Coordinated Omission 教学内容。

---

## 11. 阿里云 8C32G × 2 实战 playbook

**目标**：用两台 8 vCPU / 32G 阿里云 ECS（一台 server / 一台 client）跑完四类测试、scp 数据回本机、释放实例。**全程约 1.5 小时，单次费用 ¥5 左右**。

按 Step 0 → 6 顺序往下做，不需要回前面章节查表。

---

### Step 0：买机器 + 配安全组（控制台，10 分钟）

| 项 | 推荐 | 不要选 |
|---|---|---|
| 实例规格 | **`ecs.g7.2xlarge`** (8c32g) 或 `ecs.c7.2xlarge` (8c16g) | t6 / s6 / 共享型 / 突发性能型——CPU credit 会让数据抖动 |
| 数量 | 2 台 | — |
| OS | Ubuntu 22.04 / 24.04 LTS | CentOS（apt 没 tcpkali，要源码编） |
| 计费 | 按量付费 | 包月（用完忘释放浪费钱） |
| 网络 | 专有网络 VPC，**同 region、同 zone、同 VPC** | 跨 zone 延迟翻倍；不同 VPC 互不通 |
| 公网 IP | client 机器配（你 ssh 进来用），server 不需要 | server 暴露公网增加风险 |
| 数据盘 | 默认 40G 系统盘够用 | — |

**安全组规则**（控制台 ECS → 安全组）：

| 方向 | 协议 | 端口 | 源 | 用途 |
|---|---|---|---|---|
| 入方向 | TCP | 22 | 你自己的 IP/32 | SSH（两台都加） |
| 入方向 | TCP | 18002 | **client 内网 IP**/32 | 压测端口（仅 server 加） |
| 入方向 | TCP | 5201 | client 内网 IP/32 | iperf3（仅 server 加，临时） |

> ⚠️ **不要** 把 18002 开成 `0.0.0.0/0`——echo server 没认证，公网会被秒爆。

**拿内网 IP**（两台机器都拿，记下来）：

```bash
ip addr | grep -E "inet (10\.|172\.|192\.168\.)" | awk '{print $2}'
# 假设 server: 172.16.0.5, client: 172.16.0.6
```

---

### Step 1：两台机器都做（依赖 + 编译 + 系统调优，每台 5 分钟）

```bash
# (1) 依赖
sudo apt update && sudo apt install -y \
    build-essential cmake g++-13 \
    liburing-dev pkg-config git \
    tcpkali iperf3 htop sysstat

# (2) clone + Release 编译（必须 Release，Debug 性能砍半）
git clone <repo-url> coro_net && cd coro_net
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13
cmake --build build -j

# (3) ulimit + sysctl（§3 全套）
ulimit -n 1000000
sudo sysctl -w net.core.somaxconn=65535 \
              net.ipv4.tcp_max_syn_backlog=65535 \
              net.ipv4.ip_local_port_range="10000 65535" \
              net.core.rmem_max=16777216 \
              net.core.wmem_max=16777216 \
              net.ipv4.tcp_tw_reuse=1 \
              fs.file-max=2000000

# (4) 验证
ulimit -n                          # → 1000000
sysctl net.ipv4.tcp_tw_reuse       # → net.ipv4.tcp_tw_reuse = 1
tcpkali --version                  # → 0.4+
ls -l build/example/echo_server_coro   # 存在
```

四项任何一项不对，先排查；不要带病压测。

---

### Step 2：iperf3 验内网带宽（5 分钟）

压测的吞吐天花板就是内网带宽。先量再压，避免出"看着 QPS 很高其实撞了网卡"的乌龙。

```bash
# Server 机器：
iperf3 -s

# Client 机器（另开 SSH）：
iperf3 -c 172.16.0.5 -t 10 -P 4
```

看 receiver 端 `Bandwidth`。阿里云 g7.2xlarge 典型 **5 Gbps**。

- ≥ 1 Gbps：可以继续
- < 1 Gbps：检查实例族是否真是 g7/c7（不是 t6/s6）、安全组是否放行、是否同 zone
- ≥ 5 Gbps：万事俱备

测完 Ctrl-C 停掉 iperf3 server。

---

### Step 3：测试 A / B / C（固定 4 worker，~30 分钟）

> **关键**：默认 `CONNECT_RATE=1000` 是阿里云 SLB 安全值（见 §8）。**云上不要改**。conn ≥ 5000 时 `DURATION` 必须 ≥ 60s（爬坡 10s + 实测都要时间）。

**两个 SSH 窗口对照操作**：

```text
╭─── SSH 1: Server 机器 ────╮   ╭─── SSH 2: Client 机器 ──────────────╮
│                            │   │                                       │
│ $ cd coro_net              │   │ $ cd coro_net                         │
│ $ ./benchmark/server_      │   │ $ export SERVER_HOST=172.16.0.5       │
│       runner.sh 4          │   │                                       │
│   (4 worker，前台输出)     │   │   # 测试 A：QPS 扫描（~12 分钟）       │
│                            │   │ $ DURATION=60s ROUNDS=3 \             │
│                            │   │   CONNECTIONS_LIST="100 1000 5000 \   │
│                            │   │                     10000" \          │
│                            │   │   bash benchmark/run_qps.sh           │
│                            │   │                                       │
│                            │   │   # 测试 B：延迟（~6 分钟）            │
│                            │   │ $ DURATION=60s ROUNDS=3 \             │
│                            │   │   OPEN_LOOP_RATE=5000 \               │
│                            │   │   bash benchmark/run_latency.sh       │
│                            │   │                                       │
│                            │   │   # 测试 C：连接极限（~5 分钟）        │
│                            │   │ $ DURATION=30s \                      │
│                            │   │   CONN_LIST="1000 5000 10000 \        │
│                            │   │              20000 50000" \           │
│                            │   │   bash benchmark/run_conn_limit.sh    │
│                            │   │                                       │
│ Ctrl-C 停 server           │   │                                       │
╰────────────────────────────╯   ╰───────────────────────────────────────╯
```

**注意**：
- 这三个测试**共用同一个 server 进程**（4 worker），不要中途重启。
- Client 看到的 `[mode] REMOTE (172.16.0.5:18002, ...)` 即正常。
- 中间任何一类报 `cannot reach 172.16.0.5:18002` → server 挂了或安全组没放行，去 SSH 1 看 server 日志。

---

### Step 4：测试 D Worker 扩展性（~15 分钟）

每个 worker 数都要重启 server，client 脚本会按 Enter 推进。

```text
╭─── SSH 1: Server ──────────╮   ╭─── SSH 2: Client ─────────────────────╮
│                            │   │ $ WORKERS_LIST="1 2 4 8" \            │
│                            │   │   DURATION=60s ROUNDS=3 CONN=1000 \   │
│                            │   │   bash benchmark/run_worker_scaling.sh│
│                            │   │ [mode] REMOTE ...                     │
│                            │   │ >>> Restart with workers=1, Enter     │
│                            │   │                                       │
│ $ ./benchmark/server_      │   │                                       │
│       runner.sh 1          │   │                                       │
│   (启动 1 worker)          │   │   ← (按 Enter，跑 1 worker 测试 3 轮)  │
│                            │   │ >>> Restart with workers=2, Enter     │
│                            │   │                                       │
│ Ctrl-C; ./server_          │   │                                       │
│   runner.sh 2              │   │                                       │
│                            │   │   ← (Enter，跑 2 worker)               │
│                            │   │   ... (依次 4, 8 worker)               │
│                            │   │                                       │
│ Ctrl-C 停掉                │   │ (脚本自然结束)                         │
╰────────────────────────────╯   ╰───────────────────────────────────────╯
```

`server_runner.sh` 会自动杀掉旧实例再起新的，所以 Ctrl-C → 启新的两步够了。

---

### Step 5：收数据 + 释放（5 分钟）

```bash
# 在你的本地笔记本（不是云机）：
scp -r <ssh-user>@<client-公网-ip>:~/coro_net/benchmark/results/cloud/ \
       ./bench_results_aliyun_$(date +%F)/

ls bench_results_aliyun_*/
# 每个时间戳子目录里：env.txt + 4 个 .csv + 4 个 .log
```

**然后立刻去阿里云控制台 "释放"**（不是"停止"！停止保留数据盘继续计费）两台 ECS。

---

### Step 6：把数据填进 BENCHMARK.md

`benchmark/results/cloud/<ts>/` 里的 4 个 CSV 对应 BENCHMARK.md §二.3 ~ §二.6 的四张表：

| CSV | 填到 |
|---|---|
| `qps.csv` | §二.3 测试 A 表（每个 conn 取 ROUNDS 轮中位） |
| `latency.csv` | §二.4 测试 B closed vs open 对比 |
| `connlimit.csv` | §二.5 测试 C，找最高 0 错误的 conn 值 |
| `workers.csv` | §二.6 测试 D，算 scale 比 |

§二.8 当前是 "TBD" 占位表，跑完后用 cloud 数据替换。

---

### 排错索引（出问题先看这）

| 现象 | 去哪 |
|---|---|
| `tcpkali: command not found` | §1 装依赖 |
| `cannot reach 172.16.0.5:18002` | Step 0 安全组、Step 4 server 是否在跑 |
| iperf3 跑不到 1 Gbps | Step 2 + 控制台确认实例族不是突发型 |
| CSV 某行 `duration_s=1` | §8 排错；通常是 sysctl 没调 |
| **≥5000 conn 三轮全 0 QPS** | §8 "云厂商 SYN flood 拦截"；保持默认 `CONNECT_RATE=1000`，不要拉高 |
| `connect: cannot assign requested address` | §3 `ip_local_port_range` / `tcp_tw_reuse=1` |
| QPS 数字比 §9 预期低 10x | Release 编译？sysctl 全套？iperf3 底数 OK？|
