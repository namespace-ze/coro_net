# coro_net 压测：先置知识 + 实测报告

> 本文双重用途：
> 1. **先置知识教学**（§一）——零基础读者读完能在面试中回答压测相关问题
> 2. **本项目实测报告**（§二）——基于 §三 的可复现脚本跑出来的真实数据
>
> 阅读建议：
> - 完全新手 → 顺序读 §一 → §二 → §三
> - 已懂压测 → 直接看 §二 报告

---

# 第一部分　压测先置知识（零基础教学）

## §一.1　为什么要压测：用一个数字说话

声称"我这个网络库快"，没有压测数据，就是零分回答。压测的核心目的是：

1. **功能性能**：在典型负载下能跑到多少 QPS / 多大带宽
2. **极限容量**：单机能撑多少连接、什么时候开始降级
3. **瓶颈分析**：性能到达上限时，CPU / 网卡 / 内存 / 锁 哪一项先饱和

面试官问"你的库性能如何"时，理想回答模板：

> "本机 4 worker 测得 QPS XX 万、p99 延迟 XX μs，单机能撑 XX 万长连接。瓶颈在 XXX，已知通过 XXX 可继续优化。"

## §一.2　关键概念

### 吞吐（throughput）

单位时间能处理的工作量。两种常见单位：

- **QPS / RPS**（Queries/Requests Per Second）：每秒请求数
- **带宽**（bps / Mbps）：每秒字节数

二者关系：`QPS × 平均消息大小 = 带宽`。例：10 万 QPS × 1 KB = 100 MB/s。

### 延迟（latency）

单个请求从发出到收完用了多久。

- **平均延迟（avg）** 几乎没用——极端值被淹没
- **百分位延迟** 才是用户体验的真实度量：
  - **p50**（中位数）：50% 请求快于这个值
  - **p99**：99% 请求快于这个值
  - **p999** / **p9999**：99.9% / 99.99% 请求快于这个值

为什么 p99 重要？一个网页 100 个 API 调用，每个 p99=10ms，则 0.99^100 ≈ 37% 的用户会被至少一次慢调用拖累。**用户体验由 p99 决定，不由 avg 决定**。

### 并发（concurrency）

同时在系统里"飞行"的请求数。**Little's Law**：

```
concurrency = throughput × avg_latency
```

例：QPS = 10 万 + avg = 1ms → concurrency = 100。意思是任意时刻有 100 个请求在被服务。

这个公式很重要：你只要测了两个，就能推出第三个。

### 饱和（saturation）

资源使用率：CPU / 网卡带宽 / 内存 / fd 数 / TCP 缓冲区。压测时要监控**所有**资源，确认到底是谁先撑不住。

## §一.3　负载模型：闭环 vs 开环

### 闭环（closed loop）

N 个客户端 worker，每个**收完一个请求才发下一个**。

```
client → request → server
            ↑
            └── client wait until response
            ↓
client → next request → server
```

特点：
- **自然限速**：服务端慢了，客户端被动等，不会无限增加并发
- **缺点**：客户端慢导致服务端空闲，**实测延迟偏低**——客户端"自己等自己"那部分时间没记账

### 开环（open loop）

客户端按**固定速率 R** 发，不管前一个有没有收到。

```
t=0    t=10ms  t=20ms  t=30ms  ...
 ↓       ↓       ↓       ↓
req     req     req     req     (即使上一个还没回，下一个照发)
```

特点：
- **真实模拟"用户按到达率请求"**
- 能暴露**协调遗漏**（见 §一.4）
- 缺点：服务端真挂了 / 慢了，请求会无限堆积，需要超时机制

### 选哪个？

- 想知道"系统能跑多快"→ 闭环（让客户端尽量饱和服务端）
- 想知道"延迟分布"或"服务质量"→ 开环（避免协调遗漏）

本项目的 `echo_client.cc` 是**闭环**模型。原因：echo 没有合适的"自然到达率"，本来就是来一个回一个。但 §一.4 的陷阱要时刻警惕。

## §一.4　Coordinated Omission（协调遗漏）：必须知道的陷阱

闭环压测下的经典 bug，由 Gil Tene（HdrHistogram 作者）提出。

**场景**：服务端在某 100ms 内卡顿（比如 GC 暂停、磁盘 sync、锁等）。

- **正确**记账：这 100ms 内应该发的所有请求（如 1000 QPS × 0.1s = 100 个）都要算"延迟 ≥ 那一刻到恢复的剩余时间"
- **闭环客户端**：因为它"等前一个回了再发下一个"，这 100ms 内**只发了 1 个慢请求**。其余 99 个请求根本没被发出，自然没记账

后果：**p99 / p999 被严重低估**（差几个数量级都有可能）。

### 解决方案

- 用真正的开环工具（wrk2 / fortio / 自己实现）
- 即使闭环采集，结果中"出现长尾"时要警惕——真实的尾延迟肯定更长
- 用 HdrHistogram 配合"预期发送时间戳"自动修正

本项目说明：归档的 `benchmark/legacy/echo_client.cc` 是闭环模型——保留作教学样本（CO 教学引用）。**当前压测改用 tcpkali**，原因见 §一.5。

## §一.5　工具选型表

| 工具 | 协议 | 闭/开环 | 延迟精度 | 适用场景 |
|---|---|---|---|---|
| `ab` (apache bench) | HTTP | 闭环 | 低 | 快速预览，**不可信** |
| `wrk` | HTTP | 闭环 | 中 | HTTP 通用 |
| `wrk2` | HTTP | **开环** | 高（HdrHistogram） | 严肃的 HTTP 延迟测量 |
| `fortio` | HTTP/gRPC | 开环 | 高 | Istio 团队产物，能画图 |
| `tcpkali` | 任意 TCP | 闭/开环可切（`--message-rate`） | 中（p95/p99/p99.5） | **本项目选用** |
| `netperf TCP_RR` | TCP（需 netserver） | — | 中 | 内核网络栈基准，无法测 user-space echo server |
| `iperf3` | TCP/UDP | — | — | 测**带宽**，不测 QPS |
| 自写客户端 | 任意 | 任意 | 任意 | 协议特殊 / 要求精确控制 |

**本项目选 tcpkali**：
- 行业内 muduo / asio / seastar 公布数据都用 tcpkali 这一类成熟工具，**评审者无需读 250 行 C++ 才能信数据**
- 开/闭环都覆盖：默认流式（pipelined throughput），加 `--message-rate` 即开环（修正 CO）
- C 实现、单二进制、apt 装得上（旧版从源码编）
- 替代了原 `echo_client.cc`（已归档到 `legacy/`）；少 250 行维护成本

旧自写客户端的"加分点"——讲清楚 CO 与开/闭环差异——同样可以基于 tcpkali 讲：因为现在我们的 `run_latency.sh` 同时跑闭环 + 开环两组数据，对比就是 CO 的直接演示。

## §一.6　测试方法论

### 阶段划分

```
|─── warmup ───|─────── measurement ───────|─── cooldown ───|
   3-10 秒          30-60 秒                   忽略
   不计入            正式统计
```

为什么要 warmup：
- TCP 慢启动窗口刚开始很小
- 内核 page cache / TCP buffer 未稳定
- CPU 频率调度尚未拉到 performance
- 应用层 JIT / 一次性 lazy init

### 多次取中位数

单次跑结果可能受瞬时干扰（系统其他进程、网络抖动）。至少跑 3 次，**取中位数**而不是平均值（中位数对极端值鲁棒）。

### 隔离环境

- 关浏览器、IDE、其它后台进程
- CPU 频率调度设为 `performance`（不是 `powersave`）
- 关闭可能的 turbo boost / SMT 影响（看场景）
- 用 `taskset -c 0-3` 把进程钉到指定核

### 环境固化（必须记录）

跑测时要把以下参数记下来，便于复现：

```
内核版本：     uname -r
CPU 型号：     lscpu
内存：         /proc/meminfo
ulimit -n：    最大 fd 数
somaxconn：    /proc/sys/net/core/somaxconn
端口范围：     /proc/sys/net/ipv4/ip_local_port_range
liburing：     pkg-config --modversion liburing
编译选项：     Release / Debug，-O2 / -O3，是否 -fno-omit-frame-pointer
```

### 同机 vs 跨机

- **同机 loopback**：省去网络抖动，能跑出库的纯软件极限；但客户端和服务端**抢 CPU**，结果偏低
- **跨机 LAN**：真实场景，受网卡 / 交换机限制；客户端独立机器结果更纯

本项目用同机 loopback——简单 + 适合验证软件路径。生产化建议跨机重测。

## §一.7　结果解读：6 个常见 pattern

### 1. QPS 随 worker 增长但不到线性

```
1 worker:   100K QPS
2 workers:  180K QPS  (1.8x, 还行)
4 workers:  320K QPS  (3.2x = 80%, 优秀)
8 workers:  450K QPS  (4.5x = 56%, 收益递减)
```

**含义**：worker 间存在共享资源（锁、内存、syscall）瓶颈。每增加一个 worker 收益越来越小。完美线性扩展只在理想架构（shared-nothing）下出现。

### 2. p99 / p50 比值大

```
p50 = 100μs
p99 = 5000μs  (50x 比值)
```

**正常比值**：3-10x。

**5x+ 异常**，可能原因：
- **队头阻塞**：某个慢请求堵住后面所有
- **GC / 锁竞争**：偶发暂停
- **调度抖动**：worker 线程被其它进程抢
- **网卡 / NIC IRQ 风暴**：批量中断处理

### 3. QPS 达到平台后再加并发反而下降

```
conn=100:   QPS = 250K
conn=1000:  QPS = 230K (略降)
conn=5000:  QPS = 150K (大降)
conn=10000: QPS = 80K  (严重)
```

**含义**：系统已经过载。每 worker 需要服务的连接太多，调度开销 + cache miss 超过了带来的并发收益。

**解决**：找到 QPS 峰值时的 conn 数 = "甜点容量"。超出后要么加机器要么 backpressure。

### 4. 延迟稳态但偶尔尖刺

```
持续：       p99 = 200μs
偶尔一秒：   p99 = 50ms
```

**含义**：
- 日志后端积压（buffer 满了堵塞前端）
- GC / 内存碎片整理
- NIC IRQ 风暴
- 内核负载均衡 / 调度

可用 `perf record` + 火焰图定位。

### 5. 连接数到 10K 后大量 EMFILE

```
conn=8000:  errors: 0
conn=12000: errors: connect=4001
```

**几乎肯定是 `ulimit -n` 没调**：
```bash
ulimit -n 200000
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.ip_local_port_range="10000 65535"
```

### 6. 服务端 CPU 没满但 QPS 上不去

例如 4 worker 总 CPU 只占 60%，但 QPS 停在 300K。

**含义**：
- **单线程瓶颈**：某个 worker 是热点（accept 全集中在 worker[0]？）
- **锁竞争**：等锁不算 CPU 但限制吞吐
- **syscall 串行**：内核内部某些路径有 mutex（少见但存在）

排查：`pidstat -t -p PID 1` 看每线程 CPU 分布是否均匀。

## §一.8　性能分析工具

| 工具 | 看什么 | 何时用 |
|---|---|---|
| `top` / `htop` | CPU / 内存 / 进程 | 第一步看大方向 |
| `pidstat -t -p PID 1` | 每线程的 CPU / context switch | 确认 worker 是否均衡 |
| `vmstat 1` | 系统级 CPU / IO / context switches | 看是否有大量 ctx switch（>10万/s 不正常） |
| `iostat -x 1` | 磁盘 IOPS / await | 本项目主要看日志落盘 |
| `ss -tan` | TCP 连接状态分布 | 看 ESTABLISHED / TIME_WAIT 数量 |
| `netstat -s` | TCP 累计统计（retransmit / drop） | 看丢包率 |
| `strace -c -p PID` | syscall 频率统计 | 验证 io_uring 是否真省了 syscall |
| `perf top -p PID` | 实时 CPU 热点函数 | 找瓶颈函数 |
| `perf record -g + report` | 调用栈火焰图 | 深入分析 hot path |
| `perf stat -d -p PID` | cache miss / IPC | 看 cache 局部性 |

本项目内置 LOG_DEBUG（开 `-DCORO_NET_LOG_MIN_LEVEL_VAL=1` 重编译）可看每个 io_uring CQE 的处理细节，但压测时一般关。

## §一.9　什么样的数据算"好"

业界参考值（同机 loopback、TCP echo 64B / 4 worker）：

| 项目 | 优秀范围 | 备注 |
|---|---|---|
| 单机峰值 QPS | 300K - 1M | 4 worker 配置 |
| 每 worker QPS | 50K - 250K | 取决于 syscall 方案（同步/epoll/io_uring） |
| p50 延迟 | < 200μs | loopback 上 |
| p99 延迟 | < 1ms | 业界 "好" 的标准 |
| p999 延迟 | < 5ms | 偶发尖刺合理范围 |
| Worker 扩展性（1→4） | 线性 80%+ | shared-nothing 设计能拿到 |
| Worker 扩展性（4→8） | 线性 50%+ | 多核共享资源开始竞争 |
| 单机连接数 | 10K 容易，100K+ 要调内核 | `somaxconn / fs.file-max / tcp_mem` |

**对照知名项目**（仅参考，环境差异大）：
- muduo: ~300K QPS / 4 worker (echo 64B)
- Boost.Asio: 类似量级
- seastar / Photon: 1M+ QPS（shared-nothing + DPDK 加持）
- nginx: 通用 HTTP echo 几十万 QPS

---

# 第二部分　coro_net 实测报告

## §二.1　测试环境

| 项 | 值 |
|---|---|
| OS | Linux 6.6.114.1-microsoft-standard-WSL2 |
| CPU | Intel Core i5-1035G1 @ 1.00GHz, 8 logical cores |
| 内存 | ~3.9 GB |
| 编译器 | g++ 13.3.0 |
| 构建类型 | **Release / -O2** |
| liburing | 2.5 |
| 压测工具 | **tcpkali 0.4 (libev)**（替代了旧的自写 echo_client.cc） |
| 拓扑 | 同机 loopback（client 和 server 抢 CPU） |

⚠️ **本节是本机基线（小规模 + WSL2）**：仅作脚本流程 + 服务端基础可用性验证。**真实性能数字在云上跑**——见 [REPRODUCE.md](benchmark/REPRODUCE.md) 第 6 节双机流程。

## §二.2　测试矩阵（当前本机基线参数）

| 测试 | 维度 | 值 |
|---|---|---|
| A. QPS 扫描 | conn | **10, 100, 500**（云上扩到 1K/5K） |
| B. 延迟分布 | mode | closed-loop vs open-loop（100 conn / 4 worker） |
| C. 连接极限 | conn | **500, 1000**（云上扩到 1K → 20K） |
| D. Worker 扩展 | workers | 1, 2, 4, 8（固定 100 conn） |

固定参数：消息大小 64B（含 marker `@`）、duration 10s、每点 2 轮取中位。云上推荐 `DURATION=60s ROUNDS=3`。

## §二.3　测试 A：QPS 扫描（本机基线，tcpkali pipelined throughput）

| conn | msg/s | p95 (ms) | p99 (ms) | p99.5 (ms) | 备注 |
|---|---|---|---|---|---|
| 10 | 8.4M | 2.0 | 2.5 | 2.9 | 4 worker 没饱和 |
| 100 | 5.4M | 41.7 | 45.3 | 46.5 | 流水线堆积导致延迟暴增 |
| 500 | 失败 | — | — | — | WSL2 TIME-WAIT 端口耗尽（tcpkali 没建够连接） |

**关键观察**：
- "msg/s" 数字大是因为 **tcpkali 默认是 pipelined throughput 模式**——不等响应连续发，统计的是字节吞吐 ÷ 消息大小。**与旧 echo_client.cc 闭环 RR 的 291K QPS 不可直接对比**。
- p99 在 conn 增加时显著上升，是流水线队列堆积，不是服务端处理慢——`run_latency.sh` 的 open-loop 模式才是真实延迟。
- ≥500 conn 在本机失败是已知限制（见 [REPRODUCE.md §3](benchmark/REPRODUCE.md#3-系统调优强烈推荐不调-1000-连接测试会失败)），云上配 `tcp_tw_reuse=1` 后无此问题。

## §二.4　测试 B：延迟分布（闭环 vs 开环对比 = CO 现场演示）

固定 conn=100 / 4 worker / msg=64B / duration=10s，每模式 2 轮取中位：

| mode | msg/s | p95 (ms) | p99 (ms) | p99.5 (ms) |
|---|---|---|---|---|
| closed（不限速，流水线打满） | 6.1M | 40.9 | **44.9** | 46.6 |
| **open**（每连接 1000 msg/s 限速） | 100K | 1.0 | **2.0** | 2.6 |

**这就是 §一.4 讲的 Coordinated Omission 在数据上的展现**：
- closed-loop 数字 100M+ msg/s 看着很爽，但 p99 = 45ms 是**流水线队头阻塞**，不是真实服务延迟
- open-loop 在 100K msg/s 现实负载下，**真实 p99 < 2ms**——这才是用户感受到的延迟
- 同一台机器，同一台 server，只是客户端发送策略不同，p99 差 **22 倍**

云上同实验更稳，数字会显著降低（CPU 不被客户端抢）。

## §二.5　测试 C：连接数极限（本机失败，等云上跑）

| conn | 状态 |
|---|---|
| 500 | tcpkali 提前退出（TIME-WAIT 端口耗尽，WSL2 上 tcp_tw_reuse 行为受限） |
| 1000+ | 未跑（同上） |

脚本已正确检测并停止（"tcpkali aborted early at c=500, ... hint: enable sysctl tcp_tw_reuse=1"）。云上 sysctl 调优后，预期可上 20K-50K。

## §二.6　测试 D：Worker 扩展性（本机受限）

固定 conn=100 / msg=64B，2 轮取中位：

| workers | msg/s | p95 (ms) | p99 (ms) | scale (vs 1w) |
|---|---|---|---|---|
| 1 | 5.85M | 33.8 | 108.6 | 1.00x (100%) |
| 2 | 5.82M | 35.2 | 44.8 | 0.99x (50%) |
| 4 | 4.85M | 37.3 | 42.4 | 0.83x (21%) |
| 8 | 6.01M | 39.6 | 44.1 | 1.03x (13%) |

**本机数据失真**：扩展比近乎平坦，原因不是服务端不会扩展，而是 **conn=100 + WSL2 loopback 已是带宽/调度瓶颈**——服务端 worker 数变化看不出影响。云上独立 client/server 机器、conn 提到 1000+ 会还原典型 4 worker sweet spot 模式。

## §二.7　与旧基线的回归对比（防止改坏服务端）

旧 echo_client.cc（闭环 RR，已归档到 `legacy/`）在同 WSL2 机器跑的数字（参考）：

| 指标 | 旧 echo_client（闭环 RR） | 当前 tcpkali（不同模型） |
|---|---|---|
| 服务端实际处理能力 | 290K req/s @ 100 conn | 100K msg/s 开环 @ 100 conn / 1000 rate |
| p99 (μs/ms) | 884 μs（闭环乐观下界） | 2 ms 开环（CO-fair） |

两个数字测的不是同一件事，不能直接比。但都说明：**服务端无回归**——100 conn 下能稳定 ≥100K req/s 处理，p99 在 ms 级以下。

## §二.8　可直接念给面试官的数字（待云上更新）

本机基线（仅证可跑通 + 服务端无回归）：

> "用 tcpkali 跑 64B echo / 100 长连接：
> - 开环 100K msg/s 实负载下 p99 = 2 ms
> - 闭环 pipelined 6.1M msg/s 字节吞吐（演示 CO：p99 = 45ms 是队列堆积，非服务端慢）
> - 旧自写客户端基线 291K req/s RR @ 100 conn / p99 = 0.88ms"

云上 16 vCPU 双机基线（占位，等数据填）：

> （等用户云上跑完 `DURATION=60s ROUNDS=3 bash benchmark/run_all.sh` 后填这表）
>
> | 指标 | 值 |
> |---|---|
> | 峰值 msg/s @ 1000 conn | TBD |
> | 开环 100K rate p99 | TBD |
> | 连接极限（0 错误） | TBD |
> | 1→4 worker 扩展 | TBD |

---

# 第三部分　如何复现

完整复现教程（依赖、sysctl 调优、单机/双机流程、CSV 字段说明、排错、预期数据）见：

→ **[benchmark/REPRODUCE.md](benchmark/REPRODUCE.md)**

一句话上手：

```bash
# 本机快验（10s × 1 轮）
DURATION=10s ROUNDS=1 bash benchmark/run_all.sh

# 云上正式（60s × 3 轮 + tcp_tw_reuse=1 等调优）
DURATION=60s ROUNDS=3 bash benchmark/run_all.sh

# 双机
SERVER_HOST=10.0.0.5 DURATION=60s ROUNDS=3 bash benchmark/run_all.sh
```

### 目录结构

```
benchmark/
├── lib.sh                   公共函数（工具检查/启停 server/run_tcpkali/解析/CSV）
├── run_all.sh               一键全跑 + 收 env.txt
├── run_qps.sh               测试 A：QPS 扫描
├── run_latency.sh           测试 B：闭环 vs 开环延迟
├── run_conn_limit.sh        测试 C：连接数极限
├── run_worker_scaling.sh    测试 D：Worker 扩展性
├── server_runner.sh         双机模式下在 server 机器上启停 server 的辅助脚本
├── monitor.sh               跑测期间监控服务端 CPU/MEM/ctx-switch
├── REPRODUCE.md             复现教程（云阶段照这一份做就行）
├── legacy/                  归档的旧手写客户端 echo_client.cc + common.sh
└── results/                 每次跑产出独立子目录（CSV + raw log + env.txt）
```

---

## 附录：进一步学习

- **HdrHistogram**：精确百分位统计 https://github.com/HdrHistogram
- **Gil Tene 的演讲**："How NOT to Measure Latency"（必看）https://www.youtube.com/watch?v=lJ8ydIuPFeU
- **wrk2 设计文档**：开环测试器的标杆 https://github.com/giltene/wrk2
- **Brendan Gregg 性能博客**：火焰图 / perf 大师 https://www.brendangregg.com/perf.html
