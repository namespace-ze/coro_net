# coro_net

> 基于 **C++20 协程 + Linux io_uring + 线程池** 的底层异步网络库。

```cpp
TcpServer server({8002, "0.0.0.0"}, /*workers=*/4);
server.set_handler([](TcpConnectionPtr conn) -> Task<void> {
    Buffer buf;
    while (true) {
        ssize_t n = co_await conn->recv(buf);   // 异步 IO，不阻塞线程
        if (n <= 0) break;
        co_await conn->send(buf.retrieveAllAsString());
    }
});
server.start();
server.wait();
```

`co_await` 让异步 IO 写起来像同步。一个 worker 线程能服务上万连接，CPU 业务可丢进独立线程池跑、协程在原 worker 上恢复，整个数据路径无锁。

---

## 特性

- **C++20 协程**：lazy `Task<T>` + symmetric transfer 防爆栈；handler 协程线性书写
- **io_uring**：每 worker 一个独立 `io_uring` 实例，shared-nothing 设计，SQ/CQ 完全无锁
- **per-connection Buffer 直写**：io_uring 直接把数据写到用户 Buffer，零额外拷贝
- **通用 TimerQueue**：per-worker `timerfd` + `std::set` 排序堆 + 序列号防 ABA；`run_after / run_every / cancel`
- **空闲连接淘汰**：CircularBuffer + shared_ptr 引用计数 → 1Hz 滴答 O(1) 淘汰，O(1) 续命
- **业务线程池**：`co_await pool.submit(fn)` 把 CPU 任务异步化，协程在原 worker resume
- **muduo 风格异步日志**：前端 `LOG_INFO << ...` 流式语法 + TLS 缓存时间字符串，后端独立线程双缓冲刷盘
- **跨线程统一 eventfd**：所有跨线程唤醒走同一窄路，与 per-worker 模型一致

---

## 系统要求

| 项 | 版本 |
|---|---|
| 操作系统 | Linux 5.18+（io_uring 充分特性） |
| 编译器 | **GCC 13+** 或 **Clang 14+** |
| liburing | `sudo apt install liburing-dev` |
| Clang 用户额外 | `sudo apt install libc++-14-dev libc++abi-14-dev`（避开 clang 14 + libstdc++ 13 的 `<chrono>` consteval bug） |

---

## 快速开始

### 构建

```bash
# GCC（推荐）
cmake -B build -S .
cmake --build build -j

# 或 Clang
CC=clang-14 CXX=clang++-14 cmake -B build -S .
cmake --build build -j
```

### 跑测试

```bash
ctest --test-dir build --output-on-failure
```

9 个测试套件、约 30 个用例，覆盖：协程基础 / io_uring / Scheduler / SchedulerPool / TcpEcho / TcpServer / 线程池 / Timer / Logger。

### 跑示例

```bash
./build/example/echo_server_coro 8002 4   # 监听 8002，4 个 worker
echo "hello" | nc 127.0.0.1 8002          # 另开终端测试
tail -f echo_server_coro.*.log            # 看异步日志
```

日志格式：`YYYYMMDD HH:MM:SS.uuuuuu TID LEVEL message - file:line`

---

## API 一览

### TcpServer / TcpConnection

```cpp
#include "coro_net/tcp.hpp"

TcpServer server({port, "0.0.0.0"}, worker_threads);
server.set_idle_timeout(std::chrono::seconds(60));    // 可选：60s 空闲淘汰
server.set_handler([](TcpConnectionPtr conn) -> Task<void> {
    Buffer buf;
    while (true) {
        ssize_t n = co_await conn->recv(buf);     // 异步读到 Buffer
        if (n <= 0) break;
        co_await conn->send(buf.retrieveAllAsString());  // 异步全部发出
    }
    co_await conn->shutdown();
});
server.start();      // 拉起 worker 线程，不阻塞
server.wait();       // 等所有 worker 退出
```

### Timer（per-Scheduler）

```cpp
auto& sched = server.pool().at(0);

TimerId t1 = sched.run_after(500ms, []{ LOG_WARN << "deadline"; });
TimerId t2 = sched.run_every(5s, []{ LOG_INFO << "heartbeat"; });
sched.cancel(t1);    // O(log n)，幂等
```

### Logger

```cpp
#include "coro_net/log.hpp"

coro_net::init_logger("my_service");   // 启动后端线程
LOG_INFO  << "x=" << x << " y=" << y;
LOG_WARN  << "slow path n=" << n;
LOG_ERROR << "failed: " << err;
LOG_FATAL << "config missing";          // 刷盘 + abort

coro_net::Logger::set_global_level(LogLevel::DEBUG);  // 运行期调级别
coro_net::shutdown_logger();
```

### 业务线程池

```cpp
#include "coro_net/thread_pool.hpp"

CoroThreadPool pool("biz", 4);
pool.start();

// 在 handler 协程内：
auto result = co_await pool.submit([req]{
    return heavy_compute(req);    // 跑在业务线程，不阻塞 IO worker
});                                // 协程在原 IO worker 上 resume
co_await conn->send(result);
```

---

## 架构总览

```
┌─────────────── 用户代码 ──────────────────┐
│   TcpServer + handler 协程                │
└───────────────────────────────────────────┘
                │
┌─── Scheduler[i] (一个 worker 线程独占) ──┐
│   io_uring    ← SQ/CQ 共享内存            │
│   eventfd     ← 跨线程唤醒                 │
│   timerfd     ← TimerQueue 到期信号       │
│   ready_      ← 本轮可 resume 的协程       │
│   cross_queue ← 跨线程投递（带锁）        │
│   + TimerQueue / IdleConnectionWheel     │
└───────────────────────────────────────────┘

┌─── CoroThreadPool（业务线程） ────────────┐
│   submit(F) → SubmitAwaiter → 业务跑完     │
│   eventfd 唤醒原 IO worker → 协程 resume   │
└───────────────────────────────────────────┘

┌─── AsyncLogger（单例） ───────────────────┐
│   前端 LOG_* → 双 4MB buffer              │
│   后端独立线程 → LogFile 滚动落盘          │
└───────────────────────────────────────────┘
```

**调度模型**：每线程一个 io_uring（模型 A）。worker[0] 兼任 acceptor，新连接 round-robin 派给某个 worker。**一个连接整个生命周期绑定一个 worker**，fd / 协程 / Buffer / timer 都在该线程上，零跨线程同步。

详细原理 → [`CORO_NET_PROJECT.md`](CORO_NET_PROJECT.md)（零基础教学：协程 / io_uring / 各模块数据结构与算法）

---

## 目录结构

```
coro_net/
├── include/coro_net/
│   ├── task.hpp / fire_and_forget.hpp           协程返回类型
│   ├── io_operation.hpp                         io_uring awaiter 基类
│   ├── scheduler.hpp / scheduler_pool.hpp       单 / 多 worker
│   ├── ops.hpp + ops/*.hpp                      6 个 io_uring awaiter
│   ├── buffer.hpp / inet_address.hpp
│   ├── tcp.hpp + tcp_connection.hpp / tcp_server.hpp / idle_*.hpp
│   ├── timer/ + thread_pool.hpp / submit_awaiter.hpp
│   ├── log.hpp + log/*.hpp                      AsyncLogger 全套
│   └── io/io_uring.h                            liburing 薄封装
├── src/                                          每个 header 对应的 .cc
├── example/echo_server.cc                       最小可工作示例
└── test/test_*.cc                               9 个测试 binary
```

---

## 文档

- **[CORO_NET_PROJECT.md](CORO_NET_PROJECT.md)** —— 零基础教学：协程 / io_uring 原理 + 各模块数据结构与算法（约 1300 行）
- **[INTERVIEW_CORO.md](INTERVIEW_CORO.md)** —— 简历项目面试 37 题：技术栈对比 / 组件设计决策 / 衍生八股（TCP / OS / 协程底层 / 内存性能）

---

## 编译器兼容性

| 工具链 | 状态 | 备注 |
|---|---|---|
| GCC 13.3 + libstdc++ | ✅ 9/9 测试通过 | 默认推荐 |
| Clang 14 + libc++ | ✅ 9/9 测试通过 | 需先装 `libc++-14-dev`，CMake 自动加 `-stdlib=libc++` |
| Clang 14 + libstdc++ | ❌ | 已知 `<chrono>` consteval 不兼容，**任何**用 `<chrono>` 的程序都失败 |

CMakeLists.txt 已根据 `CMAKE_CXX_COMPILER_ID` 自动切换 flag，用户无需手动处理。

---

## 关键设计决策

为什么这么做、为什么不那么做（详见 `INTERVIEW_CORO.md` B 组）：

- **每线程一个 io_uring** vs SQPOLL / 工作窃取：SQ/CQ 完全无锁，连接绑定 worker，业界主流方案
- **per-connection Buffer 直写** vs BufferRing：少一次 memcpy，换来内存按连接独立持有
- **eventfd 统一** vs MSG_RING：路径单一，与 shared-nothing 一致；MSG_RING 的零 syscall 优势对本库吞吐场景不显著
- **per-Scheduler TimerQueue** vs 全局堆：timerfd 必须挂在某个 ring 上；纯本地操作 cache 热
- **std::set 排序堆** vs priority_queue：std::set 支持 O(log n) 按 iterator 删除，priority_queue 不行
- **TimerId 用 sequence** vs 裸指针：防 Timer 析构后地址被复用的 ABA
- **muduo 风格双缓冲日志**：前端 memcpy 入栈 buffer 极快，后端独立线程刷盘不阻塞业务

---

## License

MIT
