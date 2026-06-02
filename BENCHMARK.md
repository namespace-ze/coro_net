# coro_net 压测：先置知识 + 实测报告

> 本文双重用途：
> 1. **先置知识教学**（§一）——零基础读者读完能在面试中回答压测相关问题
> 2. **本项目实测报告**（§二）——基于 §三 的可复现脚本跑出来的真实数据
>
> 阅读建议：
> - 完全新手 → 顺序读 §一 → §二 → §三
> - 已懂压测 → 直接看 §二 报告
>
> **当前状态**：测试套件已按 **16 vCPU 64GiB × 2** 双机重构（6 类测试 A–F）。§二 数据为
> **待测占位**，等云上跑完用 `benchmark/results/` 的 CSV 回填。方法论（§一）与脚本（§三）就绪。

---

# 第一部分　压测先置知识（零基础教学）

## §一.1　为什么要压测：用一个数字说话

声称"我这个网络库快"，没有压测数据，就是零分回答。压测的核心目的是：

1. **功能性能**：在典型负载下能跑到多少 QPS / 多大带宽
2. **极限容量**：单机能撑多少连接、什么时候开始降级
3. **瓶颈分析**：性能到达上限时，CPU / 网卡 / 内存 / 锁 哪一项先饱和
4. **资源效率**：每请求花几个 syscall、每连接占多少内存——"省"和"快"同样重要

面试官问"你的库性能如何"时，理想回答模板：

> "本机 N worker 测得 QPS XX 万、p99 延迟 XX μs，单机能撑 XX 万长连接，每请求约 X 个 syscall。瓶颈在 XXX，已知通过 XXX 可继续优化。"

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
$\text{并发数} = \text{吞吐量} \times \text{平均延迟}$
这个公式很重要：你只要测了两个，就能推出第三个。

### 饱和（saturation）

资源使用率：CPU / 网卡带宽 / 内存 / fd 数 / TCP 缓冲区。压测时要监控**所有**资源，确认到底是谁先撑不住。

### 资源效率（efficiency）

光看"能跑多快"不够，还要看"每单位工作花了多少资源"。对一个 io_uring 网络库，两个最该秀的效率指标：

- **每请求 syscall 数**：传统 epoll 模型每个请求至少 `read + write` 两次系统调用（外加 epoll_wait 摊销）；io_uring 的卖点是把多个 I/O 批进一次 `io_uring_enter`，理想下每请求趋近 **< 1 次 syscall**。用 `strace -c -p <pid>` 数：直方图里若几乎只有 `io_uring_enter`、`read/write` 寥寥，就坐实了。
- **每连接内存**：单机能撑多少长连接，取决于每条连接的固定内存开销（连接对象 + 读写缓冲 + 内核 socket buffer）。量法：`(负载态 RSS − 空载 RSS) / 连接数`。这是 C100K/C1M 能力的根。

这两个数把"快"翻译成"省"——同样的 QPS，syscall 少一半、每连接省几 KB，就是单机容量翻倍的来源。本项目用**测试 F 资源探针**（`bench_resource.sh`）量这两项。

## §一.3　负载模型：闭环 vs 开环

### 先纠正一个常见误解

直觉里"闭环 = 等回应再发，开环 = 不等回应一直发"——**这个理解是错的**。

学术 / 工业界的定义是：**closed/open 指 feedback loop 是否接 server 反馈**，不是每条等不等回应。

- **闭环 (closed loop)**：客户端的发送决策**会被 server 状态影响**。server 慢 → 客户端自动慢下来。反馈通路存在。
- **开环 (open loop)**：客户端按**外部固定时钟**发，**不管 server 状态**。server 慢死了客户端也照发不误。反馈通路打开（不接）。

"等不等每条回应"是另一个独立的轴。两个轴正交：

|  | **等回应**（每条 RR） | **不等回应**（pipelined） |
|---|---|---|
| **闭环**（接 server 反馈）| 经典 RR 闭环：client `send → recv → send`，server 慢就自动等 | tcpkali 默认：流式发，socket buffer 满 `send()` 阻塞 → 也算反馈 |
| **开环**（不接 server 反馈）| 几乎不存在（违反语义） | tcpkali `--message-rate R`：定时器照表发，buffer 满就丢/堆 |

三个具体场景：

| 模型 | 典型 throughput @ 100 conn / 200μs RTT | CO 风险 | 用途 |
|---|---|---|---|
| 经典 RR 闭环（等+闭）| ≈ 500K req/s（受 RTT 限制：100 × 1/200μs）| **高**——server 卡顿期间 client 同样卡，慢请求被遗漏 | 测单连接延迟（小心 CO）|
| tcpkali pipelined 闭环（不等+闭）| ≈ 50M msg/s（受 server 极限）| 低——buffer 自然吸收抖动 | 测系统**吞吐上限** |
| tcpkali 限速开环（不等+开）| = 客户端设定的 offered load | 无 CO | 测**真实延迟 @ 固定负载** |

### 关键洞察：throughput 数字大小 ≠ 闭环开环

很多人以为"闭环数字小、开环数字大"。**错。**数字大小由**「是否让 client 饱和」**决定：

- 闭环 pipelined **故意打满** → 数字 = server 极限（很大）
- 限速开环 **故意不饱和** → 数字 = 你设的 offered load（你想多大就多大）

要让开环测**吞吐上限**，得二分查找最大 `--message-rate`：升到 server p99 超 SLO 或开始 fail 为止，那个 rate 才是"**SLO 约束下的可持续开环吞吐**"。**本项目测试 B2 就是干这个**——扫一组 offered load，找 p99 离开地板的拐点。

### 选哪个？

- 想知道"系统极限能跑多快"→ **pipelined 闭环**（让 buffer 反压自己调速）
- 想知道"业务负载下真实延迟" → **限速开环**（设固定 rate，CO 自动修正）
- 想知道"满足 p99 < Xms 下能跑多少 req/s"→ **开环 + 二分**（throughput at SLO）

本项目用 tcpkali 跑：测试 A/C 用 pipelined 闭环测吞吐/连接上限；测试 B 用闭环 + 开环对比（B1 演示 CO）再加开环负载扫描（B2 找拐点）。旧的 `legacy/echo_client.cc` 是经典 RR 闭环，CO 风险最高——所以才换 tcpkali。

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

本项目说明：归档的 `benchmark/legacy/echo_client.cc` 是闭环模型——保留作教学样本（CO 教学引用）。**当前压测改用 tcpkali**，原因见 §一.5；**测试 B 同时跑闭环 + 开环两组数据，对比就是 CO 的直接演示**。

## §一.5　工具选型表

| 工具 | 协议 | 闭/开环 | 延迟精度 | 适用场景 |
|---|---|---|---|---|
| `ab` (apache bench) | HTTP | 闭环 | 低 | 快速预览，**不可信** |
| `wrk` | HTTP | 闭环 | 中 | HTTP 通用 |
| `wrk2` | HTTP | **开环** | 高（HdrHistogram） | 严肃的 HTTP 延迟测量 |
| `fortio` | HTTP/gRPC | 开环 | 高 | Istio 团队产物，能画图 |
| `tcpkali` | 任意 TCP | 默认 pipelined-closed；`--message-rate` 开环 | 中（p95/p99/p99.5） | **本项目选用** |
| `netperf TCP_RR` | TCP（需 netserver） | — | 中 | 内核网络栈基准，无法测 user-space echo server |
| `iperf3` | TCP/UDP | — | — | 测**带宽底数**，不测 QPS |
| `strace -c` | — | — | — | 数 **syscall 频率**（测试 F 资源效率） |
| 自写客户端 | 任意 | 任意 | 任意 | 协议特殊 / 要求精确控制 |

**本项目选 tcpkali**：
- 行业内 muduo / asio / seastar 公布数据都用 tcpkali 这一类成熟工具，**评审者无需读 250 行 C++ 才能信数据**
- 开/闭环都覆盖：默认流式（pipelined throughput），加 `--message-rate` 即开环（修正 CO）
- C 实现、单二进制、apt 装得上（旧版从源码编）
- 替代了原 `echo_client.cc`（已归档到 `legacy/`）；少 250 行维护成本

> tcpkali 延迟精度只到 0.1ms 粒度。所以报"p99 ≈ 0.2ms / 亚毫秒"比报"稳定 200μs"更站得住——0.2 是它的报告桶，真实值可能在 150–249μs。要 μs 级精确得上 HdrHistogram 类工具。

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

### 要扫的轴，不是单点

一个数字说明不了全貌。严肃 benchmark 都扫这几根轴（本项目对应测试见括号）：

- **并发连接数**（测试 A / C）：吞吐和延迟随并发怎么变，找甜点和过载点
- **消息大小**（测试 E）：小包（64-256B）瓶颈在 syscall/调度 → 看 msg/s；大包（4-16KB）瓶颈在拷贝/带宽 → 看 Gbps。muduo 经典 ping-pong 图就是 throughput vs block size
- **offered load**（测试 B2）：固定连接数扫负载，找 p99 拐点 = SLO 约束下吞吐
- **worker 数**（测试 D）：吞吐 vs 核数，看扩展线性度

### 隔离环境

- 关浏览器、IDE、其它后台进程
- CPU 频率调度设为 `performance`（不是 `powersave`）
- 关闭可能的 turbo boost / SMT 影响（看场景）
- 用 `taskset` 把进程钉到指定核。16 vCPU server 建议留几个核给 NIC 软中断 / io_uring helper：
  ```bash
  taskset -c 0-11 ./build/example/echo_server_coro 18002 12   # server 占 0-11，留 12-15 给内核
  ```

### 环境固化（必须记录）

跑测时要把以下参数记下来，便于复现（`run_all.sh` 自动写进 `env.txt`）：

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

本项目正式数据用**跨机**（阿里云双机内网，client / server 各占一台 16 vCPU）。同机 loopback 仅作脚本流程验证。

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

### 5. 连接数到 64K 后大量失败

```
conn=50000: errors: 0
conn=65000: errors: connect=large
```

单 client → 单 server:port 的并发连接上限 = 客户端可用端口数 ≈ 64K（4 元组里只有 client_port 在变）。先确认 `ulimit -n` 和端口范围：
```bash
ulimit -n 1000000
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535"
```
仍要破 64K → server 多端口或 client 多源 IP（见 REPRODUCE.md §6"破 64K 连接"）。

### 6. 服务端 CPU 没满但 QPS 上不去

例如 8 worker 总 CPU 只占 60%，但 QPS 停在某值。

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
| `netstat -s` | TCP 累计统计（retransmit / drop） | 看丢包率（netdev_max_backlog 不够会 drop） |
| `strace -c -p PID` | syscall 频率统计 | **测试 F**：验证 io_uring 是否真省了 syscall |
| `perf top -p PID` | 实时 CPU 热点函数 | 找瓶颈函数 |
| `perf record -g + report` | 调用栈火焰图 | 深入分析 hot path |
| `perf stat -d -p PID` | cache miss / IPC | 看 cache 局部性 |

本项目内置 `monitor.sh`（时序 CPU/RSS/ctx-switch）+ `bench_resource.sh`（syscall 直方图 + 每连接内存）。LOG_DEBUG（开 `-DCORO_NET_LOG_MIN_LEVEL_VAL=1` 重编译）可看每个 io_uring CQE 的处理细节，但压测时一般关。

## §一.9　什么样的数据算"好"

业界参考值（同机 loopback、TCP echo 64B）：

| 项目 | 优秀范围 | 备注 |
|---|---|---|
| 单机峰值 QPS | 300K - 1M+ | 取决于核数与消息大小 |
| 每 worker QPS | 50K - 250K | 取决于 syscall 方案（同步/epoll/io_uring） |
| p50 延迟 | < 200μs | loopback 上 |
| p99 延迟 | < 1ms | 业界 "好" 的标准 |
| p999 延迟 | < 5ms | 偶发尖刺合理范围 |
| Worker 扩展性（1→半数核）| 线性 70%+ | shared-nothing 设计能拿到 |
| Worker 扩展性（半数核→满核）| 线性 50%- | 多核共享资源 + 内核软中断抢核 |
| 单机连接数 | 64K 是单 client 端口墙；破墙需多端口/多源 IP | `somaxconn / fs.file-max / ip_local_port_range` |
| 每请求 syscall（io_uring）| 趋近 < 1 | epoll 模型约 2（read+write） |
| 每连接内存 | 几 KB - 几十 KB | 决定 C100K/C1M 能力 |

**对照知名项目**（仅参考，环境差异大）：
- muduo: ~300K QPS / 4 worker (echo 64B)
- Boost.Asio: 类似量级
- seastar / Photon: 1M+ QPS（shared-nothing + DPDK 加持）
- nginx: 通用 HTTP echo 几十万 QPS

---

# 第二部分　coro_net 实测报告

> **状态：待测占位。** 下列表格的列头/schema 已就绪，数据格为 `待测`。
> 在阿里云 16vCPU 64GiB × 2 上按 §三（REPRODUCE.md §11 playbook）跑完后，用
> `benchmark/results/cloud/<ts>/` 的 CSV + `results/resource/<ts>/` 回填。

## §二.1　测试环境（云上双机，主报告数据来源）

| 项 | 值 |
|---|---|
| 实例 | 阿里云 ECS **16 vCPU / 64 GiB**（g7.4xlarge / c7.4xlarge）× 2，独立 server / client |
| OS | Ubuntu LTS（内核 ≥ 6.x）|
| 网络 | 同 VPC、同 zone 内网互通；iperf3 内网带宽 = `待测` Gbps |
| 编译器 | g++ 13，**Release / -O2** |
| liburing | `待测`（`pkg-config --modversion liburing`）|
| 压测工具 | **tcpkali 0.4 (libev)**，client 自身 16 worker（= `$(nproc)`）；测试 F 用 strace |
| sysctl | `tcp_tw_reuse=1`、`somaxconn=65535`、`ip_local_port_range=1024-65535`、`netdev_max_backlog=250000`、`ulimit -n 1000000` |
| 拓扑 | 双机内网（client 与 server 各占一台 16 vCPU）|
| 测试参数 | duration=60s，每点 3 轮取中位，msg=64B（含 latency-marker `@`）|

本节所有数据均来自 `benchmark/results/cloud/<ts>/`（云机 scp 回来的原始 CSV），可逐条核对。

## §二.2　测试矩阵

| 测试 | 维度 | 值 | 侧 |
|---|---|---|---|
| A. QPS 扫描 | conn | 100, 1000, 5000, 10000（固定 12 worker）| client |
| B1. CO 演示 | mode | closed-loop vs open-loop（100 conn）| client |
| B2. 负载扫描 | offered load | 200K→5M（100 conn × rate 2K-50K），找 p99 拐点 | client |
| C. 连接极限 | conn | 1K → 5K → 10K → 20K → 50K | client |
| D. Worker 扩展 | workers | 1, 2, 4, 8, 16（固定 1000 conn）| client |
| E. 消息大小 | msg_size | 64, 256, 1024, 4096, 16384 B（固定 12 worker / 1000 conn）| client |
| F. 资源探针 | — | 每请求 syscall 数 + 每连接内存 | **server** |

> A/B/C/E 默认固定 12 worker（16 vCPU 上的暂定值）；测试 D 跑完确认真实 sweet spot 后回填。

## §二.3　测试 A：QPS / 吞吐扫描（pipelined throughput）

> 数字含义：tcpkali 默认 pipelined 模式（流式发，marker 追踪每条消息）的总消息吞吐。**不是 RR QPS**，与旧 echo_client.cc 的 req/s 不同维度，不要直接对比。

| conn | msg/s | bw (Gbps) | p95 (ms) | p99 (ms) | p99.5 (ms) | 备注 |
|---|---|---|---|---|---|---|
| 100 | 待测 | 待测 | 待测 | 待测 | 待测 | server 轻载 |
| 1,000 | 待测 | 待测 | 待测 | 待测 | 待测 | server 中载 |
| 5,000 | 待测 | 待测 | 待测 | 待测 | 待测 | server 接近饱和 |
| 10,000 | 待测 | 待测 | 待测 | 待测 | 待测 | server 过载？ |

**怎么读这张表**：看 msg/s 在哪个 conn 档见顶、p99 在哪个档跳到秒级（pipeline 队列堆积，非 server 慢）。真实业务延迟看 §二.4 开环数据。

## §二.4　测试 B：闭环 vs 开环（CO 演示）+ 开环负载扫描（核心数据）

> **术语对照**（详见 §一.3）：
> - **closed** = tcpkali **pipelined 闭环**：不等回应连续发，buffer 满才 `send()` 阻塞（buffer 反压 = feedback loop）。**饱和测试**。
> - **open** = tcpkali `--message-rate` **限速开环**：定时器照表发，不管 server 反馈。**故意不饱和**。
> - 数字大小由"是否饱和"决定，不由闭环/开环决定。

### B1　CO 现场演示（固定 conn=100 / 12 worker / msg=64B / 60s，3 轮取中位）

| mode | msg/s | bw (Gbps) | p95 (ms) | p99 (ms) | p99.5 (ms) |
|---|---|---|---|---|---|
| closed (pipelined 饱和) | 待测 | 待测 | 待测 | **待测** | 待测 |
| open (限速 5000 msg/s/conn × 100 = 500K offered) | 待测 | 待测 | 待测 | **待测** | 待测 |

**怎么读**：两个 p99 应差出数百倍。closed 的 p99 是消息在 socket buffer / 接收队列里排队的时间，不是 server 慢；open 的 p99 才是业务用户感受到的真实延迟。这就是 Coordinated Omission 的直接演示。

### B2　开环负载扫描：找 SLO 拐点（固定 conn=100，扫 offered load）

| offered load | 实际 msg/s | p95 (ms) | p99 (ms) | fails |
|---|---|---|---|---|
| 200K (rate 2000/conn) | 待测 | 待测 | 待测 | 待测 |
| 500K (rate 5000/conn) | 待测 | 待测 | 待测 | 待测 |
| 1M (rate 10000/conn) | 待测 | 待测 | 待测 | 待测 |
| 2M (rate 20000/conn) | 待测 | 待测 | 待测 | 待测 |
| 5M (rate 50000/conn) | 待测 | 待测 | 待测 | 待测 |

**怎么读**：p99 在低负载下贴着地板（≈ 内网 RTT + 处理时间），到某档突然抬头——**那一档 offered load = SLO 约束下的可持续吞吐拐点**。这条曲线比单点"500K 下 p99=200μs"有说服力得多。想要"连接数 × 负载"二维矩阵：换 `CONN` 重跑本测试。

## §二.5　测试 C：连接数极限

固定 12 worker / msg=64B / duration=30s，单轮：

| conn | msg/s | bw (Gbps) | p99 (ms) | connect / read / write errors |
|---|---|---|---|---|
| 1,000 | 待测 | 待测 | 待测 | 待测 |
| 5,000 | 待测 | 待测 | 待测 | 待测 |
| 10,000 | 待测 | 待测 | 待测 | 待测 |
| 20,000 | 待测 | 待测 | 待测 | 待测 |
| **50,000** | 待测 | 待测 | 待测 | 待测 |

**这张表的卖点是错误列**——单机长连接通信 60 秒全程 0 错误能撑到哪个 conn 档。pipelined 模式下 p99 注定大（队列堆积），别拿来当延迟数。**注意 64K 端口墙**：单 client 默认上限 ≈ 64K，要更高见 REPRODUCE.md §6。

## §二.6　测试 D：Worker 扩展性

固定 conn=1000 / msg=64B / duration=60s，每个 worker 数 3 轮取中位（server 重启切换 worker 数）：

| workers | msg/s | p95 (ms) | p99 (ms) | scale (vs 1w) |
|---|---|---|---|---|
| 1 | 待测 | 待测 | 待测 | 1.00x（基线）|
| 2 | 待测 | 待测 | 待测 | 待测 |
| 4 | 待测 | 待测 | 待测 | 待测 |
| 8 | 待测 | 待测 | 待测 | 待测 |
| 16 | 待测 | 待测 | 待测 | 待测 |

**怎么读**：找 QPS 高且 p99 没恶化的 worker 数 = **sweet spot**（16 vCPU 上预计 8-12）。16 worker（满核）时 server worker + io_uring helper + accept 抢满核，p99 通常恶化。**这张表定下来后，回填测试 A/B/C/E 的 `WORKERS` 默认值。**

## §二.7　测试 E：消息大小扫描

固定 12 worker / conn=1000 / duration=30s，3 轮取中位：

| msg_size | msg/s | bw (Gbps) | p99 (ms) | 主导瓶颈 |
|---|---|---|---|---|
| 64 B | 待测 | 待测 | 待测 | syscall / 调度（小包）|
| 256 B | 待测 | 待测 | 待测 | 过渡 |
| 1 KB | 待测 | 待测 | 待测 | 过渡 |
| 4 KB | 待测 | 待测 | 待测 | 拷贝 / 带宽 |
| 16 KB | 待测 | 待测 | 待测 | 网卡带宽（大包）|

**怎么读**：小包 msg/s 高但 Gbps 低（瓶颈在 per-message 开销）；大包 msg/s 降但 Gbps 接近 iperf3 底数（瓶颈在带宽）。两端瓶颈不同，单一 64B 数字说明不了全貌。

## §二.8　测试 F：资源效率（每请求 syscall + 每连接内存）

server 端 `bench_resource.sh` 在稳定负载下采样：

| 指标 | 值 | 解读 |
|---|---|---|
| 主导 syscall | 待测（应为 `io_uring_enter`）| io_uring 批处理把 N 个 I/O 合一次 enter |
| 每请求 syscall 数 | 待测（目标 < 1）| = strace total ÷ 窗口内 msg 数 |
| `read`/`write` 占比 | 待测（应很低）| 若与 enter 同量级 = 没走 io_uring 路径 |
| 空载 RSS | 待测 kB | server 起来不带连接时 |
| 负载态 RSS @ N conn | 待测 kB | 施加 N 连接稳定后 |
| **每连接内存** | 待测 bytes | = (负载 − 空载) RSS / 连接数 |

**怎么读**：这是 io_uring 库最该秀的两个数。syscall 直方图几乎只有 `io_uring_enter` = 坐实了批处理；每连接内存越小，单机能撑的长连接越多（C100K/C1M 的根）。

## §二.9　可直接念给面试官的数字（待跑完回填）

> 跑完用真实数填空。模板：

> "coro_net 在阿里云 16vCPU 64GiB × 2 双机用 tcpkali（行业标准 TCP 压测工具）跑出来：
>
> - **真实业务负载 ___ offered load 下 p99 = ___ μs**（开环测试，修正 CO 后的真实延迟），p99 拐点在 ___ offered load
> - **单机 ___ 长连接 60 秒零错误**（connect / read / write 全 0）
> - **16 vCPU server sweet spot = ___ worker**：1→___ 扩展 ___x（___%），满核收益递减
> - **每请求约 ___ 个 syscall**（io_uring 批处理，几乎只见 io_uring_enter），每连接约 ___ bytes 内存
> - **管道峰值吞吐 ___ msg/s / ___ Gbps**（pipelined，闭环；非 RR QPS）
>
> 数据全部可复现，命令在 `benchmark/REPRODUCE.md`，原始 CSV 在 `benchmark/results/`。"

---

# 第三部分　如何复现

完整复现教程（依赖、sysctl 调优、单机/双机流程、CSV 字段说明、排错、预期数据）见：

→ **[benchmark/REPRODUCE.md](benchmark/REPRODUCE.md)**

一句话上手：

```bash
# 本机快验（3s × 1 轮，验脚本流程，性能数无意义）
DURATION=3s ROUNDS=1 bash benchmark/run_all.sh

# 云上正式（60s × 3 轮 + sysctl 调优），跑 A/B/C/D/E 五类
DURATION=60s ROUNDS=3 bash benchmark/run_all.sh

# 双机（client 侧）
SERVER_HOST=10.0.0.5 DURATION=60s ROUNDS=3 bash benchmark/run_all.sh

# 测试 F 资源探针（server 侧，server 已在跑）
./benchmark/bench_resource.sh <server_pid> 15 1000
```

### 目录结构

```
benchmark/
├── lib.sh                   公共函数（工具检查/启停 server/run_tcpkali/解析/CSV）
├── run_all.sh               一键全跑 A/B/C/D/E + 收 env.txt
├── run_qps.sh               测试 A：QPS 扫描
├── run_latency.sh           测试 B：CO 演示 + 开环负载扫描
├── run_conn_limit.sh        测试 C：连接数极限
├── run_worker_scaling.sh    测试 D：Worker 扩展性
├── run_msg_size.sh          测试 E：消息大小扫描
├── bench_resource.sh        测试 F：资源探针（server 端跑，syscall + 内存）
├── server_runner.sh         双机模式下在 server 机器上启停 server 的辅助脚本
├── monitor.sh               跑测期间监控服务端 CPU/MEM/ctx-switch（时序）
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
