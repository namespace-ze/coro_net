# coro_net 项目说明书

> 一句话：基于 C++20 协程 + Linux io_uring 的异步网络库，目标是用"看起来同步"的协程代码写出"epoll + 回调地狱不到的"吞吐与延迟。

---

## 一、总体架构

```
                       ┌─────────────────────────────────────────────────┐
                       │                  TcpServer                       │
                       │   set_handler / set_idle_timeout / start         │
                       │                                                  │
                       │   listen_fd  ─┐                                  │
                       │               ▼                                  │
                       │   ┌──────────────────────────────────────────┐  │
                       │   │           SchedulerPool (N workers)       │  │
                       │   │                                            │  │
                       │   │  worker[0]   worker[1]  ...  worker[N-1]  │  │
                       │   │  ┌────────┐  ┌────────┐                   │  │
                       │   │  │io_uring│  │io_uring│                   │  │
                       │   │  │buf_ring│  │buf_ring│                   │  │
                       │   │  │evt_fd  │  │evt_fd  │                   │  │
                       │   │  │ wheel  │  │ wheel  │                   │  │
                       │   │  │ ready_ │  │ ready_ │                   │  │
                       │   │  └────────┘  └────────┘                   │  │
                       │   │   accept                                   │  │
                       │   │   loop                                     │  │
                       │   └──────────────────────────────────────────┘  │
                       └─────────────────────────────────────────────────┘
                                            │
                       业务 CPU-bound 跑这里 ▼
                       ┌─────────────────────────────────────────────────┐
                       │             CoroThreadPool                       │
                       │   N 个 std::thread + MPSC 队列                   │
                       │   submit(F) 返回 awaitable → co_await            │
                       │   业务跑完 eventfd_write 唤醒源 worker            │
                       └─────────────────────────────────────────────────┘
```

**调度模型**：模型 A —— 每线程一个 `io_uring` 实例。
- worker[0] 兼任 acceptor，accept 后 round-robin 把新 conn 派给其它 worker
- 一个 conn 整个生命周期绑定一个 worker；fd / IO / 协程都在该 worker 上跑 → 零跨线程同步

**对照 mymuduo**：

| mymuduo                 | coro_net                       |
|-------------------------|-------------------------------|
| EventLoop               | Scheduler                     |
| EPollPoller             | IoUring                       |
| Channel + handleEvent   | IoOperationBase + on_complete |
| eventfd 跨线程唤醒       | MSG_RING / eventfd 兜底       |
| ThreadPool + functor    | CoroThreadPool + Task<R>      |
| onMessage(buf,...)      | `co_await conn->recv(buf)`    |
| 手写 Entry/Bucket 时间轮 | TcpServer::set_idle_timeout()  |

---

## 二、关键名词速查

| 名词 | 一句话解释 |
|------|------------|
| **co_await expr** | C++20 关键字。挂起当前协程，等 expr 完成；expr 必须是 awaitable |
| **promise_type** | 每个协程关联的"承诺"对象，编译器要求其实现 `initial_suspend / final_suspend / return_value / unhandled_exception` 等 |
| **awaiter** | 实现 `await_ready / await_suspend / await_resume` 三件套的对象，定义"挂起时怎么做" |
| **coroutine_handle** | 协程的"句柄"，可 `.resume()` 推进、`.destroy()` 销毁；用整数地址表示协程帧 |
| **Task<T>** | 本库 lazy 协程返回类型；持有协程帧，提供 `await_ready/suspend/resume` 让它能被 co_await |
| **FireAndForget** | 顶层 eager 协程类型；自动启动、自动销毁，不需要外部 await |
| **symmetric transfer** | `final_suspend` 返回 coroutine_handle 触发尾调用而非栈递归，让 `co_await` 嵌套深度不爆栈 |
| **SQE / CQE** | io_uring 的提交项 / 完成项；用户写 SQE，内核读；内核写 CQE，用户读；两者通过 mmap 共享内存零拷贝 |
| **user_data** | SQE 中的 64-bit 字段，CQE 原样回传；本库填 `IoOperationBase*` 指针，CQE 到来时还原 awaiter 并 resume 协程 |
| **multishot** | 一次提交一个 SQE，内核持续产生多个 CQE（如 accept/recv），节省 SQE 重提交开销 |
| **provide-buffers (PBUF_RING)** | 提前注册 buffer pool 给内核；recv 时不指定 buffer，内核从池中挑、CQE 告知用了哪个 ID。N 空闲连接共享 M 个 buffer，内存 O(M) 而非 O(N) |
| **MSG_RING** | io_uring 的 opcode，从一个 ring 给另一个 ring "种"一个 CQE；用于跨线程协程恢复，零锁 |
| **registered files** | 把 fd 预注册成"槽位号"，省内核 fd refcount 查表（本库当前未启用，留作 future） |
| **COOP_TASKRUN** | io_uring setup flag：内核完成 IO 后不主动中断用户态线程，等线程主动 entry 时批处理；显著降低 IRQ 开销 |

---

## 三、目录结构 + 文件职责

```
coro_net/
├── include/coro_net/                  对外公开 API
│   ├── task.hpp                       Task<T> / FireAndForget / promise_type / FinalAwaiter
│   ├── io_operation.hpp               IoOperationBase 基类（所有 io op awaiter 的父类）
│   ├── scheduler.hpp                  Scheduler（单 worker）+ SchedulerPool（N workers）
│   ├── ops.hpp                        5+1 个 awaiter: Accept/Recv/RecvIntoBuffer/Send/Timeout/Shutdown
│   ├── buffer.hpp                     用户态读写 Buffer（peek/retrieve/append；继承 mymuduo Buffer 语义）
│   ├── inet_address.hpp               IPv4 地址封装
│   ├── tcp.hpp                        TcpConnection / TcpServer / IdleEntry / IdleConnectionWheel
│   ├── thread_pool.hpp                CoroThreadPool + SubmitAwaiter
│   └── io/
│       ├── io_uring.h                 IoUring 薄封装（get_sqe / submit / wait_cqe / peek_batch_cqe）
│       └── buffer_ring.h              BufferRing：PBUF_RING 注册 + return_buffer / view
├── src/
│   ├── coroutine/io_operation.cc      prepare_common 实现；on_complete 默认行为（push handle 到 ready）
│   ├── scheduler/scheduler.cc         Scheduler::run 事件循环、跨线程 post / wake_remote / EventfdWatcher
│   ├── net/tcp.cc                     TcpConnection 三个方法 + IdleConnectionWheel tick + TcpServer 启动/停止/accept loop
│   ├── thread_pool/coro_thread_pool.cc  CoroThreadPool worker_loop + enqueue
│   └── util/circular_buffer.h         拷自 mymuduo 的 CircularBuffer，作时间轮桶容器
├── example/echo_server.cc             协程 echo 示例
└── test/test_*.cc                     7 个测试 binary，共 19 个用例（asan 验证无 UAF / 泄漏）
```

---

## 四、三个核心数据走向

### 4.1 连接建立

```
client                worker[0] acceptor                target worker[k]
------                ------------------                -----------------
connect() ─────────► AcceptAwaiter co_await
                     (multishot 单 SQE 持续)
                       │
                       ▼
                     accept CQE: new fd
                       │
                       ▼
                     idx = next_rr()
                     post_task(target, λ)
                       │
                       MSG_RING SQE
                       │
                       ▼ kernel routes CQE to target ring
                                                       eventfd_watcher / MSG_RING CQE
                                                          │
                                                          ▼ drain_cross_queue
                                                       runs λ:
                                                         tc = make_shared<TcpConnection>(fd)
                                                         wheel.register_conn(tc) (60 个桶 + 入队尾)
                                                         spawn(handler(tc))
                                                          │
                                                          ▼
                                                       handler eager 启动
                                                       co_await recv(buf) → suspends
                                                       recv SQE 进 ring
```

### 4.2 一次 RPC 请求

```
client            worker (handler 协程)                CoroThreadPool worker
------            -----------------------              ------------------------
send request ──► multishot recv CQE 携 buffer_id
                   │
                 from buffer_ring view: 1 段 span
                 memcpy 到 per-conn Buffer
                 brg.return_buffer(bid)
                 wheel.refresh(idle_entry)
                   │
                 拆帧 [4B len][hdr][args]
                   │
                 co_await pool.submit(λ)
                 (SubmitAwaiter)
                  │       └─► enqueue λ to pool MPSC queue
                  │                                       cv.wait → 取出 λ
                  │       handler suspended                 │
                  │                                       λ() {
                  │                                         req->ParseFromString(args)
                  │                                         svc->CallMethod(... done)
                  │                                         rsp->SerializeToString(body)
                  │                                         return [4B len][body]
                  │                                       }
                  │                                         │
                  │                                       src_sched->post(handle)
                  │                                         │ (业务线程无 ring)
                  │                                         ▼ eventfd_write(worker.evt_fd)
                  │ eventfd_watcher 收到 CQE
                  ▼ drain_cross_queue: ready_.push(handle)
                 resume handler
                   │
                 co_await conn->send(frame) (SendAwaiter)
                 send SQE 进本 worker ring (零跨线程)
                   │
                 send CQE → resume handler → continue loop
                  ↓
recv response ◄── kernel TCP send (no syscall this side)
```

### 4.3 空闲连接清理

```
每个 worker 持有:
   CircularBuffer<unordered_set<shared_ptr<IdleEntry>>> buckets_(60)

tick_coro (1Hz):
   ┌─────────────────────────────────────────────┐
   │ while (running) {                            │
   │     co_await TimeoutAwaiter{1s}              │
   │     buckets_.push_back(Bucket{})  ──┐        │
   │ }                                    │        │
   └──────────────────────────────────────│────────┘
                                          ▼
                              CircularBuffer 满 (60 桶)
                              push_back 覆盖最旧的桶
                                          │
                              旧桶里 shared_ptr<IdleEntry>
                              引用计数 ↓
                                          │
                              若该 entry 不被其它桶持有
                              → ~IdleEntry 触发
                                          │
                              spawn 一个关闭协程:
                                co_await conn->shutdown()
                                          │
                              shutdown CQE → handler 的下一次 recv 返回 0
                              → handler co_return
                              → shared_ptr<TcpConnection> 释放
                              → ~TcpConnection 关闭 fd
```

---

## 五、关键设计决策的"为什么"

### 5.1 为什么选模型 A（每线程一个 ring）而非 SQPOLL / 工作窃取
- **A vs B (SQPOLL)**: SQPOLL 让内核 kthread 轮询 SQ，省 syscall，延迟最低但占一个 CPU 核且违反 SINGLE_ISSUER 时返回 -EEXIST，调试复杂。教学项目优先清晰
- **A vs C (M:N 工作窃取)**: 协程跨 worker 迁移需要管理 TLS / fd 归属 / ring 切换，C++ 协程缺少 Rust tokio 那种 send/sync 语言级保证；复杂度高，收益要在 CPU-bound 远大于 IO-bound 才能体现
- **模型 A 实际经验**：Photon / seastar / libcoro 都选这个，性能和复杂度的"甜点"

### 5.2 为什么 provide-buffers CQE 处理时立刻 memcpy + 归还 buffer ring 槽位
- **保留 mymuduo Buffer 的连续语义**：协议拆帧 (`while (buf.readableBytes() >= ...)`) 依赖数据连续 + `peek` / `retrieve` 接口；如果让 Buffer 持有多个不连续 ring slot，拆帧得跨 slot 拼接，复杂度爆炸
- **memcpy 成本 ~200ns，远小于一次 syscall 节省**；4KB 以下的 RPC 帧几乎免费
- **大消息**：multishot recv 自然产生多个 CQE，每次 append 到同一个 Buffer，Buffer 自动扩容；大消息时收益依然在（避免每包一次 syscall）

### 5.3 为什么用 MSG_RING 而非 eventfd
- **MSG_RING 零锁**：源端用自己 ring 提交 SQE，目标 ring 接收一个 CQE。本质是内核内部转发，无用户态共享数据结构
- **eventfd 需要锁保护 MPSC 队列**：本库仍保留 eventfd 兜底，但仅用于"无 ring 的线程"（main / 业务线程池）唤醒；scheduler-to-scheduler 走 MSG_RING

### 5.4 为什么时间轮内置而非用户层组装
- mymuduo 的 RpcProvider 手写了 70 行时间轮代码，要求用户理解 Entry/Bucket/WeakConnectionList，跨线程要 `runInLoop` 派发。**这是网络框架职责的合理延伸，不属于业务**
- 现代框架（photon, boost.asio）都内置 idle timeout；coro_net 跟齐
- 内置后 RpcProvider 从 250 行 → 180 行，业务一行 `server.set_idle_timeout(60s)` 配置

---

## 六、性能数据（详见 PROJECT.md §4.7）

| 指标 | 趋势 |
|------|------|
| Echo 64B 1-4 线程 QPS | coro_net 略优 (~1.1-1.3×) |
| Echo 64B 高并发 QPS    | mymuduo 略优（小包 + readv-extrabuf 优势） |
| Echo 64B p99 延迟      | coro_net 显著优（**0.25× - 0.72×**） |
| mprpc RPC 中并发 QPS   | coro_net **+25% ~ +44%**（业务跑独立线程池，IO 不阻塞） |

**适用场景**：
- 业务计算 / 解码 / 序列化耗时的 RPC server（coro_net 几乎全面优于 mymuduo）
- 对尾延迟敏感的应用（CQE 批处理消除 epoll_wait → read → write 多次内核往返）

---

## 七、代码骨架对照（mymuduo vs coro_net）

### 7.1 echo server

```cpp
// mymuduo
EventLoop loop;
TcpServer s(&loop, addr, "Echo");
s.setMessageCallback([](auto conn, Buffer* buf, Timestamp){
    conn->send(buf->retrieveAllAsString());
});
s.setThreadNum(4);
s.start();
loop.loop();
```

```cpp
// coro_net
TcpServer s({port, "0.0.0.0"}, 4);
s.set_handler([](TcpConnectionPtr conn) -> Task<void> {
    Buffer buf;
    while (true) {
        ssize_t n = co_await conn->recv(buf);
        if (n <= 0) break;
        co_await conn->send(buf.retrieveAllAsString());
    }
    co_return;
});
s.start();
s.wait();
```

### 7.2 RPC handler（核心：业务跑到独立线程，IO 不阻塞）

```cpp
// mymuduo（旧版 RpcProvider::onMessage 摘要）
m_workerPool.run([this, svc, method, req, rsp, conn]{
    auto done = NewCallback(this, &RpcProvider::sendRpcResponse, conn, rsp);
    svc->CallMethod(method, nullptr, req, rsp, done);
});
// sendRpcResponse 中 conn->send 跨线程 runInLoop 派发回 conn 的 sub-loop
```

```cpp
// coro_net（handle_conn 中片段）
std::string frame = co_await m_workerPool->submit([svc, method, args] {
    auto req = ...; auto rsp = ...;
    svc->CallMethod(method, nullptr, req, rsp, new NoopDone);
    // ... 序列化成 [4B len][body]
    return frame;
});
co_await conn->send(frame);  // 协程在原 IO worker 上 resume，零跨线程
```

---

## 八、扩展阅读 / 进一步优化

- 启用 `IORING_SETUP_R_DISABLED` + worker 线程内 `io_uring_enable_rings`，启用 SINGLE_ISSUER 进一步榨性能
- 协程支持 `AsyncGenerator<T>`，让 multishot recv 自然写成 `for co_await (auto chunk : conn.recv_stream())`
- registered files：把高频复用的 listen_fd / 长连接 fd 注册成 fixed file，节省内核 refcount
- BufferRing 弹性扩容：监控 -ENOBUFS 频率，动态加桶
- 工作量均衡：连接长尾时 round-robin 退化为 least-loaded
