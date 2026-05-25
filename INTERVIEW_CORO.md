# coro_net 简历项目面试问答集

> 25+ 题，覆盖 C++20 协程、io_uring、架构设计、与 mymuduo 对比、踩坑排错五大主题。每题答案 150-400 字，可直接面试时复述。

---

## A 组：C++20 协程基础

### A1. C++20 协程的三大关键字是什么？协程函数和普通函数的本质差异？

`co_await` / `co_yield` / `co_return`。编译器看到函数体内任一个就把它编译成状态机：
1. 调用协程函数时，先在堆上 `new` 一个"协程帧"（保存局部变量、当前状态点、promise_type 实例）
2. 立即返回一个由 `promise_type::get_return_object()` 构造的对象（本库中是 `Task<T>`）
3. 真正的执行由 `std::coroutine_handle<promise_type>` 推进；`.resume()` 恢复执行，`.destroy()` 销毁帧

普通函数的栈在 return 后销毁；协程帧在堆上、由 handle 持有，可以在任意时间点 resume。这就是协程能"挂起/恢复"的根本原因。

代码对照：`coro_net/include/coro_net/task.hpp:194` 的 `Task<T>` 即典型实现。

### A2. promise_type 必须实现哪些接口？

五个：
- `get_return_object()` — 创建协程函数的返回值（本库返回 `Task<T>`）
- `initial_suspend()` — 控制初始挂起策略（返回 `suspend_always` = lazy；`suspend_never` = eager）
- `final_suspend()` — 控制结束挂起策略（决定协程帧何时被销毁）
- `return_value(T)` 或 `return_void()` — 接收 `co_return` 的值
- `unhandled_exception()` — 处理协程内未捕获异常

参考 `task.hpp:140`。

### A3. Lazy vs Eager 协程的区别？本库分别用在哪？

- **Lazy** = `initial_suspend()` 返回 `suspend_always`，协程创建后挂起，等被 await 时才执行
- **Eager** = 返回 `suspend_never`，协程创建立刻执行

本库 `Task<T>` 是 lazy：对外语义清晰，"承诺"和"兑现"分离，便于配合 symmetric transfer。`FireAndForget` 是 eager：用于顶层 spawn，没有 await 它的人，丢出去就跑。`Scheduler::spawn` 内部就是用 FireAndForget 包了一层启动 Task。

### A4. await_ready / await_suspend / await_resume 三阶段语义？

`co_await expr` 大致展开成：
```cpp
auto& awaiter = ...; // 或 expr 直接是 awaiter
if (!awaiter.await_ready()) {                  // 1) 能否免去挂起
    awaiter.await_suspend(current_coro_handle); // 2) 决定挂起方式
    // suspend point
}
auto result = awaiter.await_resume();          // 3) 恢复后取值
```

- `await_ready` 返回 true 时跳过挂起，省一次状态保存（用于"结果立刻可得"的优化）
- `await_suspend` 三种返回值：void（普通挂起）/ bool（true 挂起，false 跳过）/ coroutine_handle（symmetric transfer 立即跳到目标协程）
- `await_resume` 返回值就是 `co_await expr` 整个表达式的值

本库 `IoOperationBase` 的 awaiter 实现见 `io_operation.hpp:73`。

### A5. final_suspend 为什么必须返回 awaiter 而不是 suspend_never？symmetric transfer 解决了什么？

设想：A 协程 `co_await B`，B 协程 `co_await C`，C 完成后要让 B 继续，B 完成后让 A 继续。

如果 `final_suspend` 简单返回 `suspend_never` 并在内部 `continuation.resume()`，那么"C 恢复 B"是一次函数调用（栈深+1）；同理"B 恢复 A"又是一次函数调用（栈深+2）。N 层 co_await 嵌套就是 N 层栈深，1000 层基本爆栈。

Symmetric transfer 把"C 完成 → 跳到 B"翻译成 *尾调用* (jmp)：C 的栈帧已经销毁，B 直接拿到 CPU 控制权，栈深保持常量。实现上 `final_suspend` 返回的 awaiter 的 `await_suspend` 返回 `std::coroutine_handle<>` 类型，编译器看到这个签名就会生成 jmp 而非 call。

本库实现见 `task.hpp:79` 的 `FinalAwaiter`。配合 `test_task.cc:80` 的 `task_deep_nested_no_stack_overflow` 用例（1000 层嵌套不爆栈）验证。

### A6. coroutine_handle 的本质是什么？

它是协程帧的"指针把手"——一个轻量的 wrapper，内部就是一个 `void*` 指向堆上的协程帧。提供：
- `.resume()` — 推进协程执行（直到下一个挂起或 return）
- `.destroy()` — 销毁协程帧并释放内存
- `.done()` — 查询协程是否已经 co_return
- `.from_address() / .address()` — 与 `void*` 互转

因为它就是个指针，可以用整数表示，本库正是把它的地址塞进 io_uring SQE 的 `user_data` 字段（64 bit），CQE 回来时还原 handle 并 resume。见 `io_operation.cc:36`。

### A7. 协程帧分配在哪？能优化吗？

默认在堆上（编译器调用 `operator new`）。问题：每次协程调用一次堆分配，高 QPS 下分配/释放开销不可忽视。

C++20 提供 HALO (Heap Allocation eLision Optimization)：如果编译器能证明协程帧的生命周期不超出调用者（即 inline + 直接 await），可以把它分配到调用者的栈上，零堆分配。条件苛刻，gcc 13 / clang 17 已经支持。

本库目前不专门追求 HALO，但写法符合（lazy Task + 直接 co_await），编译器有机会优化。生产化优化方向：自定义协程 promise 的 operator new 用 thread-local 对象池。

### A8. 协程的局部变量存哪？如何确保挂起期间地址稳定？

存在协程帧里。每次 await 挂起时，编译器把"当前还活着的局部变量"序列化到帧中固定位置。同一个变量在多次挂起之间地址稳定（这是关键）。

本库 `IoOperationBase` 把 awaiter 本身（含 `handle_`、`result_` 等字段）注入 SQE 的 `user_data`，依赖的就是：awaiter 在协程帧中、协程挂起期间帧地址不变、所以 awaiter 地址不变、CQE 回来时还原指针仍有效。

注意陷阱：`auto& x = co_await foo()` 后 x 引用的临时对象可能已被销毁——临时对象的生命周期只到 *完整表达式结束*，await_resume 返回后表达式结束，x dangling。规避：写成 `auto x = co_await foo()`。

---

## B 组：io_uring 机制

### B1. io_uring 比 epoll 强在哪？

三点本质优势：
1. **批量提交 + 异步完成**：epoll 一次 `epoll_wait` 返回 N 个就绪 fd 后，你还要发 N 次 read/write syscall；io_uring 一次提交批 SQE，内核都帮你做完了，结果走 CQE 回来
2. **共享内存零拷贝**：SQ 和 CQ 是 mmap 出来的环形缓冲区，用户和内核共享访问；填 SQE 不需要 syscall，读 CQE 也不需要 syscall（除非要等待）
3. **统一的异步原语**：网络 IO / 磁盘 IO / 定时器 / 跨 ring 消息 (MSG_RING) / fd 注册 (REGISTER_FILES) 等几十种 opcode 走同一套接口

实测在 Linux 5.18+ 内核 + 现代硬件上，io_uring 在高 QPS / 高并发短包场景能比 epoll 快 30%-50%，在大文件 IO 上能比 read/write 快几倍。

### B2. SQ / CQ 共享内存模型怎么工作？

`io_uring_setup` 通过 mmap 把内核内部的两个环形缓冲区映射到用户态：
- **SQ (Submission Queue)**: 用户写 SQE，内核读
- **CQ (Completion Queue)**: 内核写 CQE，用户读

每个队列有 head / tail 两个原子下标，用 release/acquire 内存序与对端同步：
- 用户填一个 SQE → 推进 sq_tail（store release）
- 内核读 sq_tail（load acquire）→ 取 SQE 处理 → 推进 sq_head
- 内核完成 IO → 写 CQE → 推进 cq_tail
- 用户读 cq_tail → 取 CQE 处理 → 推进 cq_head

整个过程纯内存操作，不需要锁。只有"通知内核"（`io_uring_enter` syscall）和"等待"（`wait_cqe` syscall）才进入内核。开了 SQPOLL 时连这两个都可以省。

### B3. user_data 字段的作用？本库如何使用？

每个 SQE 都有一个 64-bit `user_data` 字段。内核把这个值原样回传到 CQE。它的作用是"让用户拿到 CQE 时知道这是哪个 IO 的完成"。

本库填的是 `IoOperationBase*`（awaiter 自己的 this 指针）。CQE 到达时：
```cpp
auto* op = static_cast<IoOperationBase*>(io_uring_cqe_get_data(cqe));
op->on_complete(cqe->res, cqe->flags);  // 默认实现：存结果 + push handle 入 ready_
```
然后 scheduler resume `op->handle_`，协程从 `co_await` 处继续。

这就是把"异步 IO 完成"和"协程恢复"耦合起来的关键。代码见 `scheduler.cc:233`。

### B4. multishot accept / recv 是什么？代价收益？

普通 SQE 是 single-shot：一次提交、产生一次 CQE、SQE 即用即弃，下次还要再提交。

Multishot：提交一次 SQE，内核持续产生多个 CQE，直到 SQE 被取消或资源耗尽。CQE 的 `flags` 中 `IORING_CQE_F_MORE` 标志位指示"我还会继续产生 CQE"。

**收益**：accept / recv 这类高频操作不用每次都重新填一个 SQE，节省 SQE 提交开销。长连接服务器收益最明显。

**代价**：
1. 编程模型从"每次都建一个 awaiter" 变成"生成器/订阅"，与单 await 模式不直接兼容，本库 acceptor 用到了 multishot accept（但用 single-shot 包装在 `AcceptAwaiter` 里调用），recv 暂用 single-shot
2. 内核版本要求高（multishot accept 5.19+，multishot recv 5.20+）

### B5. Provide-buffers (PBUF_RING) 解决什么真实问题？

传统 recv 必须提前给内核一个 buffer 地址。N 个连接同时收数据需要 N 个 buffer。10 万连接需要 10 万 × 4KB = 400MB 内存。

provide-buffers 把 buffer 池预注册给内核，发起 recv 时 *不指定 buffer*，由内核挑：
- 用户提前 `io_uring_register_buf_ring` 注册一个 ring，比如 1024 个 4KB buffer
- 提交 recv 时 `sqe->flags |= IOSQE_BUFFER_SELECT; sqe->buf_group = bgid`
- 内核读到数据后从 ring 挑空闲 buffer 写入，CQE 中 `cqe->flags` 高位携带 buffer ID
- 用户处理完，归还 buffer 到 ring

**收益**：10 万连接共享 1024 个 buffer，内存 O(M) 而非 O(N)，节省 99%+。

本库实现见 `buffer_ring.h` 与 `ops.hpp` 的 `RecvIntoBufferAwaiter`。

### B6. MSG_RING 比 eventfd 强在哪？

MSG_RING (5.18+) 是 io_uring 提供的"跨 ring 消息" opcode：源线程提交 SQE 到自己 ring，内核负责把一个 CQE 投递到目标 ring。

vs eventfd：
- **MSG_RING 零锁**：没有用户态共享数据结构，内核直接转发；本库 worker → worker 唤醒走这条路
- **eventfd 需要锁**：要保护 MPSC 队列（你 write eventfd 之外还要塞数据进队列让目标读到）
- **MSG_RING 一次 SQE 完成**：eventfd 至少要 write + 目标 read 两次 syscall

本库给"无 ring 线程"（main / CoroThreadPool worker）保留了 eventfd 兜底（`scheduler.cc:73` 的 `EventfdWatcher`），但 scheduler-to-scheduler 走 MSG_RING（`scheduler.cc:154` 的 `wake_remote`）。

### B7. registered files 省了什么？本库为什么没启用？

每次 io_uring_submit 都要查 fd 表、递增 fd 的 refcount、IO 完成后递减。`IORING_REGISTER_FILES_UPDATE` 把 fd 预注册成"槽位号"，后续提交时用槽位号代替 fd，省掉 fd 表查找和 refcount 操作。

对短连接（频繁打开关闭 fd）收益约 10-20%。对长连接持续 IO 也有 5% 左右收益。

本库当前没启用是因为：要管理槽位的分配/释放（连接关闭释放槽位，新连接申请槽位），代码复杂度上升不少，教学项目里没值得。后续优化清单里有。

### B8. SQPOLL 为什么不开？

SQPOLL 让内核起一个 kthread 轮询 SQ tail，用户态填完 SQE 不需要 io_uring_enter syscall 通知内核，延迟最低（亚微秒级）。

不开的原因：
1. 占一个 CPU 核（kthread 100% busy poll）
2. kernel < 5.13 需要 CAP_SYS_NICE 权限
3. 与 SINGLE_ISSUER 互动复杂（本库已经放弃 SINGLE_ISSUER）
4. 调试时 ring 状态瞬时不一致，提高排错难度

教学项目优先清晰可调试。生产追极致延迟（HFT / 存储引擎）会开。

---

## C 组：架构设计

### C1. 为什么选每线程一个 ring（模型 A）？

三个模型：
- **A**：每线程一个 ring，acceptor + N worker。SQ/CQ 完全无锁，连接绑定 worker，fd 不跨线程
- **B**：共享 ring + SQPOLL。延迟最低但 SQ/CQ 多线程竞争需要 CAS，可扩展性差
- **C**：M:N 工作窃取，多 ring + 全局任务队列。最灵活但协程跨线程迁移管理 TLS / fd 归属是深坑

A 是 Photon / seastar / libcoro / async_simple 等主流框架的默认。可扩展性、性能、复杂度的最佳平衡点。

实测中 A 在通用服务器场景吞吐和 B 持平甚至更好，可扩展性远好于 B。C 在极混合负载下理论上限最高，但 C++ 缺少 Rust tokio 那种 Send/Sync 语言级约束，工程难度大。

### C2. 连接为什么不能跨 worker 迁移？

模型 A 假设"一个连接生命周期内只属于一个 worker"。原因：
1. 该 worker 的 io_uring SQ/CQ 已经"绑定"了这个 fd 的 IO（pending SQE / 完成中的 CQE）
2. 该连接的 awaiter 对象、协程帧、per-conn Buffer、idle entry 都在该 worker 的内存视野下，跨线程访问需要同步
3. io_uring 跨 ring 发送 SQE 涉及内核侧引用计数 + 复杂的 cancel 协议

代价：如果某个 worker 上分配的长连接特别活跃，其他 worker 闲着，会出现负载不均（round-robin 退化）。
缓解方向：后续可以加 least-loaded 调度（在派发时看每个 worker 的 `pending_handlers()` 数）。

### C3. acceptor 单线程会成为瓶颈吗？

理论上：单 listen socket 的 accept 是串行的（内核里有一把锁）。压测显示约 30 万-50 万 accept/s 单核可达。一般 RPC 服务的连接建立频率远低于此。

如果真成瓶颈：开 `SO_REUSEPORT` + 多 listen socket，每个 worker 自己 accept（mymuduo 之前也是单 acceptor）。但这种用法 N 个 listen 之间负载不一定均衡，要看内核版本。

本库当前是单 acceptor on worker[0]，绝大多数场景够用；进一步优化路径明确。

### C4. 时间轮为什么内置在网络库而非应用层？

原版 mymuduo 的 RpcProvider 手写了 70+ 行时间轮代码：用户要懂 Entry / Bucket / WeakConnectionList、`runEvery(1s, ...)` 注册定时器、跨 sub-reactor 用 `runInLoop` 派发。这些不属于业务逻辑，属于"网络框架职责的合理延伸"。

现代框架（photon、boost::asio 的 connection_timeout）都内置。coro_net 跟齐：
```cpp
server.set_idle_timeout(60s);  // 一行配置
```

收益：
- RPC 层从 250 行 → 180 行，业务零代码
- 测试聚焦：时间轮的正确性测试归 coro_net，不再混在 RPC 测试里
- 跨线程派发由库内部协程恢复机制处理（不再 runInLoop）

### C5. 业务线程池和 IO 线程怎么交互？

`CoroThreadPool` 是独立的 std::thread 集合，跑 CPU-bound 工作（protobuf 反射 / 序列化 / 业务逻辑）。

衔接方式：`co_await pool.submit(λ)` 返回 `SubmitAwaiter`。
1. `await_suspend(handle)` 记下 `src_sched = Scheduler::current()`，把 λ + 完成通知打包进 std::function 入队
2. CoroThreadPool worker 取出 λ，执行，把结果存到 awaiter 的 result_ 字段
3. 调 `src_sched->post(handle)`：业务线程没 ring → 走 eventfd 兜底（`scheduler.cc:147`），write 8 字节
4. IO worker scheduler 的 `EventfdWatcher` 读到 → CQE 到达 → 醒来 drain cross queue → push handle 到 ready_
5. 下一轮 resume handle → `await_resume` 读 result_ 返回值

关键：**协程在原 IO worker 上 resume**（不是在业务线程！）。所以后续的 `conn->send` 走的是原 worker 的 ring，fd 没有跨线程使用，io_uring SQ 也无锁。

### C6. 优雅停机怎么做？

`TcpServer::stop()` 链：
1. `running_ = false`（atomic）
2. `shutdown(listen_fd, SHUT_RDWR)` —— 让 acceptor 的 `AcceptAwaiter` CQE 收到 `-ECANCELED` 自然退出
3. 对每个 worker：`post_task(λ { sched.stop(); })`
   - post_task 跨线程派发 + wake_remote（这里调用线程是 main，无 ring，走 eventfd）
4. 每个 worker 在自己的 run loop 顶部检测 `stopping_`，跑完 ready_ 后退出
5. `pool.wait()` join 所有 worker 线程

在途的 handler 协程：当 listen_fd 关闭后不会有新连接；现有连接的 recv 在某一轮可能返回 `-ECANCELED` 或 0，handler 自然 co_return；协程帧析构，shared_ptr<TcpConnection> 释放，~TcpConnection 关闭 fd。

弱点：当前没显式 cancel 在途 IO，依赖 listen_fd / conn_fd 关闭让内核自然取消。生产化可以提交 `IORING_OP_ASYNC_CANCEL` 主动取消。

---

## D 组：与 mymuduo 对比

### D1. epoll → io_uring 的具体收益数据？

详见 `PROJECT.md §4.7`。三个场景的关键数字：

| 场景 | mymuduo | coro_net | 备注 |
|------|---------|----------|------|
| Echo 64B / 1 线程 QPS | 10,258 | 11,380 (+11%) | 单连接 syscall 节省 |
| Echo 64B / 4 线程 p99 | 435 μs | 152 μs (**−65%**) | CQE 批处理消除回调跳转 |
| RPC 50B / 4 线程 QPS | 15,031 | 21,665 (**+44%**) | 业务跑独立线程池，IO 不阻塞 |
| RPC 50B / 64 线程 p99 | 4,030 μs | 4,146 μs | 业务 CPU 已饱和，IO 优势打不出来 |

**最大收益方向**：尾延迟 + 业务 CPU-bound 场景。**反方向**：纯小包高并发 echo（mymuduo 的 readv + 64KB 栈 extrabuf 一次读尽设计在小包高并发场景仍有优势）。

### D2. 回调风格 → 协程风格，代码量怎么变化？

RpcProvider 的对比（同一个功能：拆帧 → 业务 → 回包）：

```cpp
// mymuduo 版（节选；总共要写 onConnection / onMessage / sendRpcResponse / Entry / Bucket / onTimer 六处）
void RpcProvider::onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp){
    while (...) { // 拆帧
        m_workerPool.run([this, svc, method, req, rsp, conn]{
            auto done = NewCallback(this, &RpcProvider::sendRpcResponse, conn, rsp);
            svc->CallMethod(method, nullptr, req, rsp, done);
        });
    }
}
void RpcProvider::sendRpcResponse(const TcpConnectionPtr& conn, Message* rsp){
    std::string frame = ...; conn->send(frame); // conn->send 内部 runInLoop 派发
}
```

```cpp
// coro_net 版（一个函数搞定，业务逻辑像同步代码）
Task<void> RpcProvider::handle_conn(TcpConnectionPtr conn) {
    Buffer buf;
    while (true) {
        ssize_t n = co_await conn->recv(buf);
        if (n <= 0) co_return;
        while (拆帧) {
            std::string frame = co_await m_workerPool->submit([&]{ ... CallMethod ... });
            co_await conn->send(frame);
        }
    }
}
```

代码量：250 行 → 180 行（约 −28%）。可读性提升更大：业务流"recv → submit → send"线性，无回调嵌套，无 runInLoop 派发，无时间轮 Entry/Bucket 心智负担。

### D3. 你的 Buffer 改造和原 muduo 有什么不同？

完全复用 mymuduo Buffer 的接口（peek / readableBytes / writableBytes / retrieve / append），只删除了 `readFd` / `writeFd`（这两个是同步 syscall 包装，被 io_uring awaiter 替代）。

新增配套：`RecvIntoBufferAwaiter`，使用 `IORING_REGISTER_PBUF_RING` 的 provide-buffers：
1. 不指定 recv 缓冲区，让内核从 BufferRing 挑一个槽位写
2. CQE 携带 `buffer_id`，我们 memcpy 数据到 per-conn Buffer（这步保留 Buffer 的连续数据语义）
3. 立即归还 buffer 槽位

代价：每个 packet 多一次 memcpy（~200ns / 4KB）。收益：N 个空闲连接共享 M 个 ring buffer，内存 O(M)。

### D4. 性能瓶颈从哪里转移到了哪里？

mymuduo 长连接版的瓶颈分析（见 PROJECT.md §4.5）：
- 瓶颈 1: 协议解析 + protobuf 反射（每次 RPC ~50 μs 固定开销）
- 瓶颈 2: 跨线程派发（sub-reactor → worker → conn 的 sub-loop，runInLoop + eventfd）

coro_net 干掉了瓶颈 2 大头：
- 协程在原 IO worker 上 resume，没有 runInLoop
- 业务线程 → IO worker 的恢复只有一次 eventfd write

剩下的瓶颈 1（protobuf 反射）依然存在，所以 64 线程高并发下 QPS 持平。

新增的"成本"：
- 协程帧分配（HALO 优化前）每协程 200-500 B 堆分配
- BufferRing memcpy
- multishot 暂未用（用了的话会进一步降低 SQE 提交开销）

### D5. 哪些场景反而协程不如回调？

1. **极简短包高并发 echo**：mymuduo 的 readv + 64KB 栈 extrabuf 设计能一次 syscall 读尽，coro_net 当前的 RecvIntoBufferAwaiter 限制单次 4KB（虽然不到上限就 return，但 wake-up 次数仍可能多）
2. **业务零开销 / 不解码**：协程帧分配相对显著
3. **关心瞬时延迟分布的尾百分位**：CQE 批处理是双刃剑——平均延迟下降，但偶尔批次大时尾延迟上升

实际工程中这些场景属于"小众极端"。RPC server / 网关 / 数据库连接处理这类典型场景，协程 + io_uring 全面占优。

---

## E 组：踩坑与排错

### E1. 协程生命周期 dangling 怎么避免？

最常见两种坑：

**坑 1**：栈对象引用被协程帧捕获。
```cpp
Task<void> bad() {
    int x = 1;
    return [&x](TcpConnectionPtr conn) -> Task<void> { co_await conn->send(...); }(conn); // 错！x 已退栈
}
```
规避：协程帧内的变量直接写在协程函数体里（成为帧的一部分），不要从外面引用。

**坑 2**：`auto& y = co_await foo()` 后 y 引用临时对象，表达式结束后 dangling。
规避：`auto y = co_await foo()` 拷贝/move 到协程帧。

**坑 3**（本项目实测踩中）：`std::make_shared<IdleEntry>(IdleEntry{c, sched})` 中的临时 `IdleEntry{...}` 在 make_shared 内部 move 之后被析构。如果析构函数有副作用（本库里 ~IdleEntry 会触发 conn 关闭），就会误触发。
规避：用显式构造函数 `make_shared<IdleEntry>(c, sched)` 直接在堆上构造，不走临时对象。

工具：编译期 `-Werror=dangling-reference`；运行期 `-fsanitize=address`（asan）。本项目 S6 阶段就是用 asan 抓到 use-after-free。

### E2. final_suspend 写错会怎样？

最容易写错的是返回 `suspend_never` + 内部 resume continuation：
```cpp
// 错误版本
struct FinalAwaiter {
    bool await_ready() noexcept { return false; }
    void await_suspend(coroutine_handle<Promise> h) noexcept {
        h.promise().continuation_.resume();  // 函数调用 → 栈深 +1
    }
    void await_resume() noexcept {}
};
```

后果：N 层 co_await 嵌套 → N 层栈深 → 大约 1000 层就爆栈。本库 `test_task.cc:80` 的 `task_deep_nested_no_stack_overflow` 用例就是验证这点。

正确版本是 symmetric transfer：`await_suspend` 返回 `coroutine_handle<>`，编译器生成 jmp，栈深常量。见 `task.hpp:79`。

### E3. multishot recv 怎么探测连接断开？

multishot recv 持续产生 CQE 直到：
- `cqe->res == 0`：peer 发了 FIN，对端正常关闭
- `cqe->res < 0`：错误（如 `-ECONNRESET`、`-ETIMEDOUT`）
- `!(cqe->flags & IORING_CQE_F_MORE)`：multishot 本身终止（可能是 res<=0 也可能是资源问题）

应用层逻辑：
```cpp
while (auto chunk = co_await conn->recv_stream()) {  // 假设 AsyncGenerator
    if (chunk.empty()) break;  // EOF
    process(chunk);
}
```

本库当前用 single-shot recv，处理同样简单：`co_await recv(buf)` 返回 0 表示 EOF；返回 < 0 表示错误。

### E4. buffer ring 用尽（-ENOBUFS）怎么兜底？

CQE 返回 `-ENOBUFS` 表示提交时所有 buffer 都在用、内核没空闲 buffer 写入。本库当前的策略是简单的"重新提交"——awaiter 把 -ENOBUFS 当作可重试错误返回给协程，上层决定重试或放弃。

生产级方案：
1. 监控 ENOBUFS 频率
2. 自适应扩容 BufferRing（重新注册更大的 ring）
3. 提高 multishot 分散度（不要全部 conn 都长期占用 buffer）

本库教学版没做自适应，BufferRing 默认 1024 个 4KB（4MB / worker），6 worker 也就 24MB。对万级并发够用。

### E5. asan 报 use-after-free 第一步怎么查？

按 asan 报告的两个时间点定位：
1. **freed by here** 栈：释放发生在哪
2. **previously allocated by here** 栈：分配发生在哪

然后看"现在的 read/write"栈：访问发生在哪。

本项目实战：S6 阶段集成测试时 asan 报 UAF。
- 释放：post_task lambda 的 std::function 在 drain_cross_queue 结束时析构 → 释放捕获的 `handler` std::function（lambda closure）副本
- 分配：accept loop 中 `Handler handler = server->handler_;` 的 std::function 拷贝
- 访问：handler 的协程体执行时通过 `this`（lambda closure 地址）访问捕获的 `&biz`

分析：lambda 协程的协程帧持有 `this = &handler_closure`。我们把 `handler` 拷贝进了 post_task 的 captures，lambda 退出时这个副本被销毁，但协程帧仍指向这个副本 → UAF。

修复：post_task 不拷贝 handler，改为捕获 `TcpServer*`，每次通过 `srv->handler_` 访问（原始 handler 的生命周期与 server 一致）。改完 1 行，问题解决。

教训：lambda-as-coroutine 时，closure 对象生命周期必须 ≥ 协程帧生命周期。

### E6. 怎么调试 CQE 风暴定位发送方？

高并发下 CQE 几千个/秒，从 `cqe->user_data`（一个指针）看不出是哪个 op 类型，调试困难。

解决：在 user_data 高 8 bit 编码 op_type（x86_64 用户态地址只用低 47 bit，高 17 bit 空闲）：
```cpp
uintptr_t data = reinterpret_cast<uintptr_t>(this) | (op_type << 56);
```
CQE 处理时先取高位看是哪种 op，再 clear 高位还原 awaiter 指针。日志可读性大幅提升。

本库教学版没做这层编码（user_data 就是裸指针），但 MSG_RING wake 用 user_data=0 作为"忽略 CQE"的标记（`scheduler.cc:240`），这是简化版的 tag。

### E7. SINGLE_ISSUER 为什么会 EEXIST？怎么解决？

SINGLE_ISSUER flag 要求"提交 SQE 的线程必须是创建 ring 的线程"。本库 Scheduler 在 main 线程构造（构造时创建 ring），但 run() 在 worker 线程跑（即 SQE 提交在 worker），违反约束，io_uring_enter 返回 `-EEXIST`。

排查路径：
1. asan / valgrind 无信号
2. 检查 errno 17 = EEXIST，搜内核源码或文档发现 SINGLE_ISSUER 的约束
3. 解决：要么不开 SINGLE_ISSUER（本库选这条），要么用 `IORING_SETUP_R_DISABLED` 让 ring 创建时不立即可用，在 worker 线程 `io_uring_enable_rings()` 启用——以 worker 线程作为"创建者"

本项目第一次跑 `test_scheduler_pool` 时就踩中这个坑，去掉 SINGLE_ISSUER 后正常。代码注释见 `io_uring.h:62`。

---

## 附录：项目自我介绍话术（30 秒电梯演讲）

> 我重写了一个 RPC 框架的网络层：从经典多 Reactor + epoll + 同步 IO + 回调风格，切到 C++20 协程 + Linux io_uring + 异步 IO + 协程函数。新库叫 coro_net，目录与原 mymuduo 同级，通过 SchedulerPool 管理 N 个 worker（每个独占一个 io_uring 实例 + buffer ring），用 multishot accept、provide-buffers、MSG_RING 跨 ring 唤醒三个 io_uring 高阶特性把 syscall 量降一个数量级。RPC 业务通过 CoroThreadPool 跑 CPU 工作，协程在原 IO worker 上 resume，零跨线程同步。最终在中并发 (4-16 线程) 下 RPC QPS 提升 25-44%，p99 延迟普遍降到原来的 25-70%。这个项目对我最大的收获是把"看起来像同步的协程代码"和"零开销的异步 IO"绑在一起，业务代码从回调嵌套 + 跨线程 runInLoop 派发，变成读起来线性的 co_await 链；同时也踩了协程帧生命周期、lambda closure 寿命、SINGLE_ISSUER 约束等几个典型坑，用 asan 抓 use-after-free 是关键定位手段。
