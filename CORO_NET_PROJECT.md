# coro_net 项目说明书

> 一个基于 **C++20 协程 + Linux io_uring + 线程池** 的底层异步网络库。
>
> 本文档目标：零基础读者读完能掌握三块核心技术（协程 / io_uring / 跨线程协作）的原理，并能读懂仓库源码。

阅读路径建议：
1. **从未接触过协程或 io_uring** → 顺序读 §一 → §二 → §三
2. **熟悉 epoll/线程，第一次看协程** → 跳过 §一.2 io_uring 演进史，直接看 §一.3 C++20 协程
3. **想直接读源码** → §二 架构图 + §三 各模块对应文件

---

# 第一部分　项目用到的技术（零基础教学）

## §一.1　为什么需要异步网络库？从最朴素的服务器开始

想象你写一个最简单的 echo 服务器：

```cpp
int listen_fd = socket(...); bind(...); listen(...);
while (true) {
    int conn = accept(listen_fd, ...);       // ① 阻塞，等新连接
    char buf[1024];
    ssize_t n = read(conn, buf, sizeof buf); // ② 阻塞，等数据
    write(conn, buf, n);                     // ③ 阻塞，等发出去
    close(conn);
}
```

**问题**：这是单连接服务器。`accept` 在等连接时，已建立的连接也没人服务；`read` 在等数据时，新连接也接不进来。

### 一种朴素方案：每连接一个线程

```cpp
while (true) {
    int conn = accept(...);
    std::thread([conn] {
        char buf[1024];
        while (true) {
            ssize_t n = read(conn, ...);
            if (n <= 0) break;
            write(conn, buf, n);
        }
        close(conn);
    }).detach();
}
```

每个连接独立运行，互不阻塞。**问题在哪？**

- **线程贵**：每个 OS 线程默认占 8 MB 虚拟地址空间（栈），1 万个连接 = 80 GB 虚拟内存
- **切换贵**：线程切换要进内核、保存全套寄存器、刷 TLB，约 1-3 μs
- **可扩展性差**：1 万连接 = 1 万线程，调度器忙得调度自己

业界共识：**「一个 OS 线程服务很多连接」**。要做到这点，read/write 不能再阻塞——这就是"异步 IO"和"事件驱动"。

### 经典方案：epoll + Reactor

```cpp
int epfd = epoll_create1(0);
epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, ...);

while (true) {
    epoll_event evs[64];
    int n = epoll_wait(epfd, evs, 64, -1);  // 一次性拿到所有"就绪"的 fd
    for (int i = 0; i < n; ++i) {
        int fd = evs[i].data.fd;
        if (fd == listen_fd) {
            int conn = accept(...);
            epoll_ctl(epfd, EPOLL_CTL_ADD, conn, ...);
        } else {
            read(fd, ...);   // 不会阻塞——因为内核告诉我们 fd 已就绪
            write(fd, ...);
        }
    }
}
```

`epoll_wait` 一次告诉你"哪些 fd 现在可以读 / 写而不阻塞"，然后你对这些 fd 做 `read/write` —— 这次 `read/write` 不会阻塞。一个线程就能服务上万连接。

**但是**业务代码变得难写：所有逻辑都要写成回调，比如「读完请求 → 调用业务函数 → 写回响应」要拆成三段，挂在不同的 epoll 事件里，跨段共享状态要用结构体存。Node.js 的"回调地狱"就是这么来的。

### 更新的方案：io_uring + 协程

**io_uring**（Linux 5.1+）是 epoll 的下一代：
- 不仅告诉你"哪些 fd 就绪"，而是**直接帮你做完 IO**，结果异步返回
- 用户态 / 内核态共享内存队列，无 syscall 也能批量提交、批量收割

**协程**（C++20）让异步代码**写起来像同步**：

```cpp
Task<void> handler(TcpConnectionPtr conn) {
    Buffer buf;
    while (true) {
        ssize_t n = co_await conn->recv(buf);   // 看起来同步，实际异步
        if (n <= 0) break;
        co_await conn->send(buf.retrieveAllAsString());
    }
}
```

`co_await` 表示"挂起当前协程，等 IO 完成再恢复"——挂起期间不占线程；恢复后从挂起点继续。

**两者结合**：io_uring 提供异步原语，协程提供同步外观。本项目要做的事就是把这两块绑在一起。

---

## §一.2　Linux 异步 IO 的演进：select → epoll → io_uring

如果你只想用 io_uring，可以跳到 §一.3。但理解演进史能帮你说清"io_uring 强在哪"。

### syscall 是什么

`read / write / socket / epoll_wait` 都是 **syscall**（系统调用）：用户态程序请求内核做事的接口。一次 syscall 大致流程：

```
用户态 →（保存寄存器 + 切换权限级 + 进入内核）→ 内核执行 → 返回用户态
        ~50-100 ns 纯开销
```

**减少 syscall 就是性能优化的核心方向之一**。

### 第一代：select / poll

```c
fd_set readfds;
FD_SET(fd1, &readfds); FD_SET(fd2, &readfds); ...
select(maxfd+1, &readfds, ...);
```

每次都要把所有 fd 数组传给内核（用户态 → 内核态拷贝），内核线性扫描所有 fd 看哪个就绪。**N 个 fd 每次 syscall 都是 O(N)**。1024 个 fd 上限。

### 第二代：epoll（Linux 2.6）

`epoll_create` 创建一个内核数据结构（红黑树 + 就绪链表）。`epoll_ctl` 注册 fd（O(log N)）。`epoll_wait` 只返回**已经就绪**的 fd（O(1)，内核维护就绪链表）。

```
用户                       内核
----                       ----
epoll_create1()  ───►      创建 epoll 实例（红黑树 rbr_ + 就绪链表 rdllist_）
epoll_ctl(ADD)   ───►      插入红黑树；注册 wakeup 回调
                            │
                            └─► fd 数据到达 → 内核 wakeup 把 fd 放入 rdllist_
epoll_wait()     ───►      返回 rdllist_ 内容（拷贝）→ 用户拿到就绪 fd
read(fd, buf, n) ───►      实际读数据（一次 syscall）
```

但 epoll 仍要为每次 IO 做一次额外的 `read/write` syscall。10 万次 IO 就是 20 万次 syscall（epoll_wait + read）。

### 第三代：io_uring（Linux 5.1+）

**核心思想**：用户和内核**共享内存**，避免反复的用户/内核数据拷贝。

```
                 用户进程                     内核
                 ────────                    ─────
   ┌──────────┐                         ┌──────────┐
   │ SQ ring  │  ←─ 用户填提交项 SQE     │          │
   │ (mmap)   │  ←─ 内核读 SQE 处理 ────►│  io_uring│
   │          │                         │   实例    │
   │ CQ ring  │  ←─ 内核写完成项 CQE ───►│          │
   │ (mmap)   │  ←─ 用户读 CQE          └──────────┘
   └──────────┘
```

- **SQ (Submission Queue)**：用户写"我要做什么 IO"的 SQE，内核读
- **CQ (Completion Queue)**：内核写"做完了，结果是这个"的 CQE，用户读

两个队列都是 mmap 出来的环形缓冲区。**填 SQE 和读 CQE 都不需要 syscall**（只是写共享内存）。只有"告诉内核来取一批 SQE"和"等 CQE 到来"才需要 syscall（`io_uring_enter` / `wait_cqe`），而且一次 syscall 可以批处理几十到几千个 IO。

一次完整的 IO：

```
1. io_uring_get_sqe()           // 用户：从 SQ 拿一个空 SQE（纯内存）
2. io_uring_prep_recv(sqe, ...) // 用户：填好"recv fd / buf / len"
3. io_uring_sqe_set_data(sqe, ctx) // 用户：填一个 64-bit user_data
4. io_uring_submit()            // 用户：syscall 通知内核来取（多个 SQE 一起）
5. (内核异步执行 recv)
6. (内核写 CQE 到 CQ ring)
7. io_uring_wait_cqe(&cqe)      // 用户：syscall 等至少一个 CQE
8. cqe->res / cqe->user_data    // 用户：拿到结果和上下文
9. io_uring_cqe_seen(cqe)       // 用户：标记 CQE 已处理（纯内存）
```

第 1-3、8-9 步全是用户态内存操作，零 syscall。第 4、7 步可以批处理多个 IO。

### epoll vs io_uring 性能直觉

10 万次 IO：
- epoll：10 万次 epoll_wait + 10 万次 read = 20 万 syscall
- io_uring：~1000 次 submit_and_wait（batch size 100）= 1000 syscall

理论上 200 倍 syscall 数量差异（实际收益受 batch 大小、IO 类型影响，通常 30-50%）。

---

## §一.3　C++20 协程：从直觉到实现

### 协程是什么

**普通函数**：调用进入，return 后栈帧销毁，下次调用是全新的开始。

**协程**：调用进入，可以「暂停」（`co_await`），返回控制权给调用方但**保留所有局部变量**；之后可以「恢复」继续从暂停处往下执行。

直觉类比：游戏存档。普通函数是"一气呵成的关卡"；协程是"打到 boss 前可以存档，去吃个饭，回来继续打"。

### C++20 协程的三大关键字

```cpp
co_await expr;   // 暂停当前协程，等 expr 完成，恢复后取 expr 的结果
co_yield val;    // 生成器：产出 val 并暂停，调用方下次推进时继续（本库未用）
co_return val;   // 结束协程，返回值给调用方
```

**编译器看到函数体内任一关键字就把这个函数变成状态机**：

```cpp
Task<int> compute() {
    int x = 1;
    int y = co_await fetch_data();   // 暂停点 1
    int z = co_await fetch_more(y);  // 暂停点 2
    co_return x + y + z;
}
```

编译器大约会展开成（伪代码）：

```cpp
struct compute_frame {
    promise_type promise;        // promise_type 决定协程的行为
    int x, y, z;                 // 所有跨暂停点的局部变量
    int state;                   // 当前在哪个暂停点
};

Task<int> compute() {
    auto* f = new compute_frame{};   // 协程帧在堆上（可被 HALO 优化到栈上）
    f->x = 1;
    f->state = 0;
    /* ... 状态机展开 ... */
    return f->promise.get_return_object();   // 返回 Task<int>
}
```

**关键点**：协程帧在**堆上**，由 `std::coroutine_handle` 持有；函数调用结束栈销毁后，协程帧依然存在，可以稍后通过 handle 恢复执行。

### Awaiter：定义"暂停时怎么做"

`co_await expr` 要求 `expr` 是一个 **awaiter**——实现以下三个方法的对象：

```cpp
struct Awaiter {
    bool await_ready();
        // 返回 true 表示"结果已经准备好，不用挂起"，跳过下面两步
    void await_suspend(std::coroutine_handle<> h);
        // 决定挂起时做什么：通常是把 h 存起来，等异步操作完成时调用 h.resume()
    T await_resume();
        // 协程恢复后调用，返回值就是 co_await expr 整个表达式的值
};
```

### 一个最小可工作的例子

```cpp
// 一个永远准备好的 awaiter，await_resume 返回 42
struct Immediate {
    bool await_ready() { return true; }
    void await_suspend(std::coroutine_handle<>) {}
    int await_resume() { return 42; }
};

Task<int> demo() {
    int x = co_await Immediate{};   // x == 42
    co_return x;
}
```

### Promise type：协程的"配置中心"

每个协程函数有一个关联的 `promise_type`，由 `Task<T>::promise_type` 给出。它告诉编译器：

```cpp
template <typename T>
struct TaskPromise {
    Task<T> get_return_object();             // 怎么创建 Task<T> 返回给调用方
    auto initial_suspend();                  // 协程刚创建时是否挂起
    auto final_suspend() noexcept;           // 协程结束时怎么处理
    void return_value(T v);                  // co_return v 调用这里
    void unhandled_exception();              // 协程里抛了异常怎么办
};
```

本库 `Task<T>` 的设计选择：
- `initial_suspend()` 返回 `std::suspend_always` → **lazy**，协程创建后**不立刻执行**，等被 `co_await` 才启动
- `final_suspend()` 返回一个特殊的 `FinalAwaiter` → 用 **symmetric transfer** 防止深层 `co_await` 嵌套爆栈

### Symmetric transfer：防爆栈的关键

设想 A `co_await` B，B `co_await` C，C 完成后要恢复 B，B 完成后要恢复 A。

**朴素方式**：C 完成后调用 `B_handle.resume()` 这是一次函数调用（栈深 +1）；B 完成后调用 `A_handle.resume()`（栈深 +2）。1000 层嵌套就 1000 层栈深，爆栈。

**Symmetric transfer**：让 `final_suspend()` 返回的 awaiter 的 `await_suspend` 返回**目标协程的 handle**：

```cpp
struct FinalAwaiter {
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
        return h.promise().continuation_;   // 返回"在等我的那个协程"的 handle
    }
    void await_resume() noexcept {}
};
```

编译器看到 `await_suspend` 返回 `coroutine_handle<>`，会把"跳到目标 handle"翻译成 **jmp 而非 call**——尾调用优化，栈深保持常量。本库 `task.hpp:91` 即此实现。`test_task.cc` 中的 `task_deep_nested_no_stack_overflow` 用 1000 层嵌套验证。

### FireAndForget：顶层协程入口

`Task<T>` 是 lazy 的——必须有人 `co_await` 才会执行。但事件循环要启动一个**没人 await** 的协程（比如新连接的 handler），怎么办？

引入 `FireAndForget`：
- `initial_suspend()` 返回 `suspend_never`（创建立刻执行）
- `final_suspend()` 返回 `suspend_never`（结束自动销毁帧）

`Scheduler::spawn(Task<void>)` 内部就用 `FireAndForget` 包一层启动。

---

## §一.4　线程池为什么不能省

你可能问：协程在一个 worker 线程上跑，io_uring 让 IO 不阻塞，为什么还要线程池？

**答案**：协程**只解决 IO 阻塞**，不解决 CPU 阻塞。如果 handler 协程要做一次 5 ms 的解压缩 / 加密 / 业务计算：

```cpp
Task<void> handler(TcpConnectionPtr conn) {
    auto data = co_await conn->recv(buf);
    auto plain = decompress(data);   // ← 5ms 同步 CPU 工作
    co_await conn->send(plain);
}
```

这 5 ms 内，**整个 worker 线程被这一个协程独占**——这个 worker 上的所有其他连接的 IO 全都被串行化。

解决方案：把 CPU 工作扔到独立的**业务线程池**：

```cpp
auto plain = co_await pool.submit([data]{ return decompress(data); });
```

- `pool.submit` 把 lambda 入队，业务线程取出执行
- handler 协程在 `co_await` 处挂起，让出 worker，IO 不再被阻塞
- 业务线程跑完后，**唤醒原 IO worker 上的协程恢复执行**（这就是后面 §三.10 要讲的"跨线程恢复"）

---

# 第二部分　总体架构

## §二.1　模块全景

```
   ┌─────────────────── 用户代码 ─────────────────────┐
   │   TcpServer s({port, "0.0.0.0"}, 4);            │
   │   s.set_handler([](TcpConnectionPtr c) -> Task  │
   │       { ... });                                  │
   │   s.start(); s.wait();                           │
   └──────────────────────────────────────────────────┘
                          │
                          ▼
   ┌─────────────────── TcpServer ───────────────────┐
   │   listen_fd + SchedulerPool + handler 模板       │
   └──────────────────────────────────────────────────┘
                          │
   ┌─────────────────── SchedulerPool ───────────────┐
   │     N 个 Scheduler，每个跑在自己的 std::thread    │
   └──────────────────────────────────────────────────┘
                          │
   ┌─ Scheduler[i] (一个 worker 线程独占) ────────────┐
   │                                                  │
   │   ┌──────────┐                                   │
   │   │ IoUring  │ ← SQ/CQ 共享内存与内核通信         │
   │   ├──────────┤                                   │
   │   │ eventfd  │ ← 跨线程唤醒入口（其他线程 write） │
   │   ├──────────┤                                   │
   │   │ timerfd  │ ← TimerQueue 的到期信号           │
   │   ├──────────┤                                   │
   │   │ ready_   │ ← std::deque<coroutine_handle<>>  │
   │   │ 队列      │   本轮可立即 resume 的协程        │
   │   ├──────────┤                                   │
   │   │ cross_   │ ← 跨线程投递来的协程 handle/task  │
   │   │ queue    │   （锁保护，event 循环 drain）    │
   │   └──────────┘                                   │
   │                                                  │
   │   附加组件：                                      │
   │   • TimerQueue       通用定时器（per-Scheduler）  │
   │   • IdleConnWheel    专用空闲连接淘汰时间轮       │
   └──────────────────────────────────────────────────┘
                          │
   ┌─ TcpConnection (handler 协程持有 shared_ptr) ──┐
   │   recv / send / shutdown 三个 Task<>            │
   │   Buffer + idle_entry（弱引用）                  │
   └──────────────────────────────────────────────────┘

   ┌─────────── CoroThreadPool（业务线程） ──────────┐
   │   N 个 std::thread + MPSC 队列                   │
   │   submit(F) 返回 SubmitAwaiter（可 co_await）    │
   │   业务跑完 → eventfd_write 唤醒原 IO worker      │
   └──────────────────────────────────────────────────┘

   ┌─────────── AsyncLogger（单例） ─────────────────┐
   │   前端 LOG_INFO << ... → 双 4MB buffer           │
   │   后端独立线程 → LogFile 滚动落盘                 │
   └──────────────────────────────────────────────────┘
```

## §二.2　调度模型：每线程一个 io_uring

**核心选择**：N 个 worker 线程，每个独占一个 `io_uring` 实例。

为什么这样选：
- **SQ/CQ 完全无锁**：每个 ring 只被一个线程访问
- **fd 与线程绑定**：一个连接的所有 IO 都走同一个 ring，CPU cache 友好
- **协程不跨线程迁移**：协程帧只在创建它的 worker 上 resume，跨 worker 时通过"投递 handle + 唤醒目标 worker"实现，逻辑边界清晰

worker[0] 兼任 acceptor：它跑 accept 循环，每个新连接 round-robin 派给某个 worker（包括自己）。

## §二.3　核心交互：一次完整的连接生命周期

```
时间 →

main 线程           worker[0] (acceptor)        worker[k] (handler)
─────────           ────────────────────        ──────────────────
TcpServer s(...)
s.start():
  pool.start()      Scheduler::run() 开始
  queue_boot_task   ┌─ tls_current_ = this
                    ├─ rearm_eventfd_watch
                    ├─ timer_queue_->start
                    └─ 进入主循环
                    drain_cross_queue:
                       boot task 跑 → spawn accept_loop()
                                        │
                                        ▼
                       accept_loop: co_await AcceptAwaiter
                                       │（挂起，SQE 进 ring）
                                       │
                       client connect ─┤
                       ◄─ accept CQE ──┘
                                       │ accept_loop 恢复
                                       │
                       idx = round_robin
                       post_task(worker[k], λ_create_conn)
                          │
                          cross_.tasks.push(λ)
                          write(worker[k].wake_fd, 1)
                                                   eventfd 可读
                                                   ◄─ EventfdWatcher CQE
                                                   drain_cross_queue:
                                                     λ_create_conn 跑:
                                                       make_shared<TcpConnection>
                                                       wheel.register_conn
                                                       spawn(user_handler(conn))
                                                                  │
                                                                  ▼
                                                   user_handler 立即启动（FireAndForget）
                                                   co_await conn->recv(buf)
                                                      │（挂起，recv SQE 进 ring）
                                                      │
                       client send "hi" ──────────►   │
                                                   ◄─ recv CQE (n=2)
                                                   handler 恢复
                                                   buf.hasWritten(2)
                                                   co_await conn->send("hi")
                                                      │
                                                   ◄─ send CQE
                                                   handler 恢复，循环 co_await recv 又挂起
                                                      ⋮
                       client close ──────────────►
                                                   ◄─ recv CQE (n=0, EOF)
                                                   handler co_return
                                                   ~TcpConnection 关闭 fd
                                                   协程帧释放

s.stop():
  shutdown(listen_fd)
  post_task(worker[k], λ: sched.stop)               eventfd CQE
                                                   stopping_ = true
                                                   下一轮主循环退出
s.wait() → join all
```

---

# 第三部分　各模块深入：数据结构 + 算法

## §三.1　Task<T> 与协程协议（`task.hpp` / `fire_and_forget.hpp`）

### 数据结构

```cpp
template <typename T>
class Task {
    using promise_type = TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;
    Handle handle_;   // 持有协程帧的"指针把手"，move-only
};

template <typename T>
struct TaskPromise {
    std::variant<std::monostate, T, std::exception_ptr> result_;
    //         未完成    co_return 的值   抛出的异常
    std::coroutine_handle<> continuation_ = std::noop_coroutine();
    //   ↑ "正在等我的那个外层协程"——用于 symmetric transfer 跳回
};
```

### 关键算法：co_await 一个 Task

当外层协程写 `co_await some_task`：

```
1. Task::await_ready()
   返回 !handle_ || handle_.done()。lazy task 创建后 done=false，故返回 false → 必须挂起

2. Task::await_suspend(caller_handle)
   handle_.promise().continuation_ = caller_handle;  // 记下外层
   return handle_;  // ★ 返回自己 → 编译器生成 jmp 直接跳到 self 入口执行

3. (self 协程跑到 co_return)
   自动触发 final_suspend → FinalAwaiter::await_suspend → return continuation_;
   → jmp 跳回外层

4. Task::await_resume()
   读 result_：若是异常分支则 rethrow_exception，否则返回值
```

整条路径**没有一次函数调用栈递增**，N 层嵌套不爆栈。

### FireAndForget 与 Task 的差异

| | Task | FireAndForget |
|---|---|---|
| initial_suspend | suspend_always (lazy) | suspend_never (eager) |
| final_suspend | FinalAwaiter (跳回外层) | suspend_never (自动销毁) |
| 谁来 await | 必须有外层 co_await | 没人 await |
| 用途 | 业务协程逻辑 | 顶层 spawn 入口 |

`Scheduler::spawn(Task<void> t)` 内部用一个 shim：

```cpp
namespace {
FireAndForget spawn_shim(Task<void> t) {
    co_await std::move(t);
    co_return;
}
}
void Scheduler::spawn(Task<void> task) { spawn_shim(std::move(task)); }
```

`spawn_shim` 是 FireAndForget，立即执行；内部 `co_await` 把传入的 Task 启动。

---

## §三.2　IoOperationBase：把 io_uring 操作包成 awaiter

### 数据结构（`io_operation.hpp`）

```cpp
class IoOperationBase {
protected:
    Scheduler* sched_;                       // 关联的 Scheduler（决定 SQE 进哪个 ring）
    std::coroutine_handle<> handle_;         // 等我完成的协程
    int32_t  result_ = 0;                    // CQE 的 res 字段
    uint32_t cqe_flags_ = 0;                 // CQE 的 flags 字段
public:
    virtual void on_complete(int32_t res, uint32_t flags) noexcept;
    void prepare_common(io_uring_sqe* sqe, std::coroutine_handle<> h);
    // await_ready / await_resume 默认实现
};
```

### 算法：一次 IO 的生命周期

以 `RecvAwaiter` 为例（`ops/recv.hpp`）：

```cpp
class RecvAwaiter : public IoOperationBase {
    int fd_;  void* buf_;  size_t len_;
public:
    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        io_uring_prep_recv(sqe, fd_, buf_, len_, 0);
        prepare_common(sqe, h);   // 内部：handle_=h; io_uring_sqe_set_data(sqe, this);
    }

    ssize_t await_resume() noexcept { return result_; }
};
```

提交到 CQE 处理的全链路：

```
co_await RecvAwaiter{fd, buf, len}
   │
   ▼
await_suspend(h):
   1. sqe = ring.get_sqe()        从 SQ 拿一个空槽
   2. io_uring_prep_recv(sqe,...) 填 op = RECV, fd, buf, len
   3. handle_ = h                 记下当前协程
   4. sqe->user_data = this       ★ 把 awaiter 自己的地址塞进 user_data
   (协程挂起，事件循环继续)

事件循环 submit_and_wait
   │
   ▼ 内核做 recv，CQE 写入 CQ
   │
   ▼ submit_and_wait 返回，peek_batch_cqe 拿到一批 CQE
   for cqe in cqes:
       op = (IoOperationBase*) cqe->user_data;
       op->on_complete(cqe->res, cqe->flags);
                ↓
       默认实现：
         result_ = res;
         cqe_flags_ = flags;
         sched_->push_ready(handle_);   ← 协程进 ready 队列

事件循环顶部：处理 ready 队列 → handle_.resume()
   │
   ▼ co_await 表达式恢复
await_resume() 被调用 → 返回 result_ 给协程，作为 co_await 表达式的值
```

**关键点**：`awaiter` 对象本身在协程帧里、挂起期间地址稳定，所以把 `this` 塞 user_data 是安全的。CQE 来时从 user_data 还原 awaiter 指针，访问 `handle_` 拿到协程把手，resume。

### 6 个具体 awaiter（`ops/*.hpp`）

| awaiter | io_uring op | 用途 |
|---|---|---|
| `AcceptAwaiter` | `OP_ACCEPT` | 接受新连接 |
| `RecvAwaiter` | `OP_RECV` | 读到指定 buffer |
| `RecvIntoBufferAwaiter` | `OP_RECV` | 读到 per-conn `Buffer`（io_uring 直写） |
| `SendAwaiter` | `OP_SEND` | 写指定 buffer |
| `TimeoutAwaiter` | `OP_TIMEOUT` | 等待一段时间（一次性、不可取消） |
| `ShutdownAwaiter` | `OP_SHUTDOWN` | 半关闭 socket |

---

## §三.3　IoUring 薄封装（`io/io_uring.h`）

只做 5 件事：
- 构造：`io_uring_queue_init_params` 创建 ring，启用 `IORING_SETUP_COOP_TASKRUN`（完成时不主动中断用户态线程，省 IRQ 开销）
- `get_sqe()` → 从 SQ 拿空槽
- `submit()` → 通知内核来取（一次 syscall）
- `submit_and_wait(n)` → 提交并阻塞等至少 n 个 CQE
- `peek_batch_cqe()` → 一次拿一批 CQE

构造时**不**启用 `IORING_SETUP_SINGLE_ISSUER` —— 这个 flag 要求"创建 ring 的线程必须是提交 SQE 的线程"，而本库 Scheduler 在 main 线程构造、worker 线程 run，违反约束会返回 -EEXIST。

---

## §三.4　Scheduler：worker 的"大脑"（`scheduler.hpp` / `scheduler.cc`）

### 数据结构

```cpp
class Scheduler {
    std::unique_ptr<IoUring>    ring_;         // 本 worker 的 io_uring
    std::unique_ptr<TimerQueue> timer_queue_;  // 通用定时器
    std::deque<std::coroutine_handle<>> ready_;  // 本轮可恢复的协程

    int wake_fd_ = -1;                         // eventfd，跨线程唤醒入口
    class EventfdWatcher;                      // 永久挂在 ring 上的 read SQE
    std::unique_ptr<EventfdWatcher> wake_watcher_;

    struct CrossQueue {                        // 跨线程投递队列（带锁）
        std::mutex mu;
        std::vector<std::coroutine_handle<>> handles;
        std::vector<std::function<void()>> tasks;
    } cross_;

    static thread_local Scheduler* tls_current_;  // 当前线程关联的 Scheduler
};
```

### 算法：事件循环

```cpp
void Scheduler::run() {
    tls_current_ = this;
    rearm_eventfd_watch();          // 挂上 read(wake_fd, 8B) 等跨线程唤醒
    timer_queue_->start();          // 挂上 read(timerfd, 8B) 等定时器到期

    while (!stopping_) {
        drain_cross_queue();        // 1) 取跨线程投递的 handle/task
        while (!ready_.empty()) {   // 2) resume 本轮可恢复的协程
            auto h = ready_.front(); ready_.pop_front();
            if (h && !h.done()) h.resume();
        }
        if (stopping_) break;

        int r = ring_->submit_and_wait(1);   // 3) 提交+等至少 1 个 CQE
        // 4) 批量处理 CQE
        io_uring_cqe* cqes[64];
        unsigned n = ring_->peek_batch_cqe(cqes, 64);
        for (unsigned i = 0; i < n; ++i) {
            void* d = io_uring_cqe_get_data(cqes[i]);
            if (!d) continue;       // stop() 的 NOP SQE 走这里
            ((IoOperationBase*)d)->on_complete(cqes[i]->res, cqes[i]->flags);
        }
        ring_->cq_advance(n);
    }
    tls_current_ = nullptr;
}
```

### 跨线程通信：eventfd 统一路径

外部线程（main / 其他 worker / 业务线程池）要让本 Scheduler 做事：

```cpp
void Scheduler::post(std::coroutine_handle<> h) {
    if (tls_current_ == this) { push_ready(h); return; }    // 本线程直接入队
    { std::lock_guard lk(cross_.mu); cross_.handles.push_back(h); }
    wake_remote();   // 跨线程：写 eventfd 通知
}

void Scheduler::wake_remote() {
    if (tls_current_ == this) return;
    uint64_t v = 1;
    ::write(wake_fd_, &v, sizeof v);
}
```

`EventfdWatcher` 是一个永久挂在 ring 上的 `read(wake_fd, 8B)` IoOperation 子类。eventfd 一旦可读，CQE 立刻到达，事件循环醒来执行 `drain_cross_queue`。`on_complete` 内自我 re-arm 一次新的 read SQE，循环往复。

---

## §三.5　SchedulerPool：N 个 worker（`scheduler_pool.hpp`）

```cpp
class SchedulerPool {
    std::vector<std::unique_ptr<Scheduler>> schedulers_;
    std::vector<std::thread> threads_;
    std::atomic<size_t> next_idx_{0};   // round-robin 计数器
};

void SchedulerPool::start() {
    for (auto& s : schedulers_)
        threads_.emplace_back([p = s.get()] { p->run(); });
}

Scheduler& SchedulerPool::next() {
    return *schedulers_[next_idx_.fetch_add(1) % schedulers_.size()];
}
```

简单到只是个容器。复杂性都在每个 Scheduler 自己处理。

---

## §三.6　TcpServer / TcpConnection（`tcp_server.hpp` / `tcp_connection.hpp`）

### TcpServer：用户视角的入口

```cpp
class TcpServer {
    InetAddress addr_;
    int listen_fd_ = -1;
    SchedulerPool pool_;
    Handler handler_;       // function<Task<void>(TcpConnectionPtr)>
    std::chrono::seconds idle_{0};
    std::vector<std::unique_ptr<IdleConnectionWheel>> wheels_;
};
```

`start()` 的关键步骤：
1. `socket + bind + listen` 拿到 `listen_fd_`
2. 为每个 worker 构造 `IdleConnectionWheel`（若开了 idle timeout）
3. 在 worker[0] 上排一个 boot task：spawn 一个 `accept_loop` 协程
4. `pool_.start()` 拉起所有 worker 线程

`accept_loop` 协程：

```cpp
Task<void> accept_loop(TcpServer* srv, Scheduler& s) {
    while (srv->running_) {
        AcceptAwaiter aw{srv->listen_fd_, s};
        int conn = co_await aw;             // 异步等连接
        if (conn < 0) break;                // listen_fd 被 shutdown，正常退出
        size_t idx = pick_worker(conn);     // round-robin
        srv->pool_.at(idx).post_task([srv, conn, peer = aw.peer(), idx]() {
            // 在目标 worker 线程上执行
            auto tc = std::make_shared<TcpConnection>(conn, peer, srv->pool_.at(idx));
            if (srv->wheels_[idx]) {
                auto entry = srv->wheels_[idx]->register_conn(tc);
                tc->install_idle(srv->wheels_[idx].get(), entry);
            }
            Scheduler::current()->spawn(srv->handler_(std::move(tc)));
        });
    }
}
```

### TcpConnection：单连接

```cpp
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
    int fd_;
    InetAddress peer_;
    Scheduler* sched_;                     // 强绑定的 worker
    IdleConnectionWheel* wheel_;           // 可空
    std::weak_ptr<IdleEntry> idle_entry_;  // 时间轮里的标记

public:
    Task<ssize_t> recv(Buffer& buf);       // 直接调 RecvIntoBufferAwaiter
    Task<ssize_t> send(std::span<const char> data);  // 内部循环 send 直到全发完
    Task<void>    shutdown();
};
```

**线程亲和**：`TcpConnection` 整个生命周期只在 `sched_` 这一个线程上跑。所有方法（包括析构 `close(fd_)`）都隐含此约束 → 无需锁。

---

## §三.7　Buffer：应用层读写缓冲（`buffer.hpp`）

### 数据结构：muduo 风格的三段式

```
 |  kCheapPrepend(8B)  |  prependable  |  readable  |  writable  |
 0                  readerIndex_                writerIndex_     size

 vector<char> 实际存储，readerIndex_ / writerIndex_ 是下标
```

- **readable**：已收到、待应用消费的数据
- **writable**：可继续 append 新数据的空闲区
- **kCheapPrepend**：固定 8 字节保留区（协议拆帧时可以在前面廉价 prepend 4B 长度）

### 关键算法

```cpp
void ensureWritableBytes(size_t len) {
    if (writableBytes() < len) makeSpace(len);
}

void makeSpace(size_t len) {
    if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
        // 整体不够 → 扩容
        buffer_.resize(writerIndex_ + len);
    } else {
        // 整体够，只是 prependable 占太多 → memmove 数据到 kCheapPrepend 起点
        size_t readable = readableBytes();
        std::memmove(begin() + kCheapPrepend, begin() + readerIndex_, readable);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
    }
}

// io_uring 直写支持
void hasWritten(size_t n) noexcept {
    assert(n <= writableBytes());
    writerIndex_ += n;
}
```

`RecvIntoBufferAwaiter::await_suspend`：

```cpp
buf_->ensureWritableBytes(4096);
io_uring_prep_recv(sqe, fd_, buf_->beginWrite(), buf_->writableBytes(), 0);
// CQE 来时：buf_->hasWritten(res);
```

io_uring 把数据直接写到 `beginWrite()`，零拷贝，0 次 memcpy。

---

## §三.8　IdleConnectionWheel：空闲连接淘汰（`idle_connection_wheel.hpp`）

**问题**：1 万个长连接挂在那不发数据，要全部 60 秒后自动关闭。怎么扫描"哪些连接 60 秒没活动"？

**朴素方案**：每个连接记一个 `last_active_time`，定时扫描所有连接 → O(N) 每秒。

**时间轮方案**：把"超时时刻"分桶，每秒推进一桶。

### 数据结构

```cpp
class IdleConnectionWheel {
    using Bucket = std::unordered_set<std::shared_ptr<IdleEntry>>;
    util::CircularBuffer<Bucket> buckets_;   // 例如容量 60 = idle_seconds
    Scheduler* sched_;
};

struct IdleEntry {
    std::weak_ptr<TcpConnection> wconn;
    Scheduler* sched;
    ~IdleEntry();    // 析构时关闭对应连接
};
```

### 算法

```
buckets_  =  [B0, B1, ..., B59]   // 队头是"最旧"
                            ▲
                            └─ 新连接 / 续命 都插到队尾
```

每个连接持有 `shared_ptr<IdleEntry>`，**只能** 出现在队尾桶里。`shared_ptr<IdleEntry>` 的引用计数刚好等于"它出现在多少个桶里"。

**1Hz 滴答**：每秒调用 `buckets_.push_back(Bucket{})`。CircularBuffer 满 60 后 push_back 会**覆盖最旧的桶**——该桶里所有 `shared_ptr<IdleEntry>` 引用计数 -1。如果某个 entry 不再出现在任何桶里，引用计数归零，析构触发，关闭对应连接。

**续命**：连接有数据来时（recv 成功），把它的 `shared_ptr<IdleEntry>` 重新插到队尾桶 —— 引用计数 +1，下一秒的桶覆盖不会让它归零。

### 时序图

```
t=0   : 连接 A 建立 → entry_A 插入 B0 (队尾)
        buckets_: [B0={A}, B1={}, ..., B59={}]
                          shared_count(A) = 1

t=1   : push_back 空桶 → buckets_: [B1={}, ..., B59={}, B60={}]
        B0 被推出（CircularBuffer 队头出队）→ entry_A 引用计数 -1 = 0
        ~entry_A → 关 A

t=2   : 但如果 t=0.5 A 有数据：
        wheel.refresh(entry_A) → 把 entry_A 插到当时队尾 B0
        现在 entry_A 出现在 B0 一处
        ...
        到 t=60 时 B0 被推出，refcount 才归 0 → 60 秒空闲才关
```

**关键性能**：1Hz tick 是 O(1)（一次 push_back + 一个桶的引用计数批量 -1），续命也是 O(1)（哈希集合插入）。**整体扫描复杂度 O(1) 与连接数无关**。

### IdleEntry 析构语义

```cpp
IdleEntry::~IdleEntry() {
    auto c = wconn.lock();
    if (!c || !sched) return;
    LOG_DEBUG << "IdleEntry evicting fd=" << c->fd();
    sched->spawn([](TcpConnectionPtr c_) -> Task<void> {
        co_await c_->shutdown();
        co_return;
    }(c));
}
```

不能直接 `c->shutdown()` —— shutdown 是 Task<void>，要 co_await，析构里没法 co_await。所以 spawn 一个壳协程。

---

## §三.9　TimerQueue：通用定时器（`timer/timer_queue.hpp`）

`TimeoutAwaiter` 是一次性的（co_await 之后就消费掉），`IdleConnectionWheel` 是专用的（只服务连接淘汰）。要做"500ms 后调 fn"、"每 5 秒打一行日志"、"取消之前注册的 timer"，需要通用定时器。

### 数据结构

```cpp
class TimerQueue {
    Scheduler* sched_;
    int        timerfd_ = -1;
    class TimerfdWatcher;             // 类似 EventfdWatcher 的永久 read SQE
    std::unique_ptr<TimerfdWatcher> watcher_;

    std::atomic<TimerSequence> next_seq_{1};
    std::set<std::pair<TimePoint, Timer*>> timers_;  // 主索引：按到期排序
    std::unordered_map<TimerSequence, Timer*> active_;  // 辅助：O(1) seq→timer 查找

    bool calling_expired_ = false;
    std::vector<TimerSequence> canceling_seqs_;
};

class Timer {
    std::function<void()> callback_;
    TimePoint     expiration_;
    Duration      interval_;     // 0 = 一次性；>0 = 重复
    bool          repeat_;
    TimerSequence sequence_;     // 单调递增，防 ABA
};
```

### 算法 1：add

```cpp
TimerId TimerQueue::add(cb, delay):
    when = now() + delay
    seq  = ++next_seq_
    t    = new Timer{cb, when, 0ns, seq}
    was_top = timers_.empty() || when < timers_.begin()->first
    timers_.insert({when, t})       // O(log n)
    active_[seq] = t                // O(1)
    if (was_top):
        reset_timerfd(when)          // 让 timerfd 在 when 时刻触发
    return TimerId{t, seq}
```

`reset_timerfd` 把 `delay` 当作相对时间传给 `timerfd_settime`，注意 `{0, 0}` 是 disarm 不是立即触发，要 clamp 到 `{0, 1}`。

### 算法 2：handle_expired（CQE 到达时）

```cpp
void TimerQueue::handle_expired():
    now = steady_clock::now()
    expired = []
    while timers_.begin()->first <= now:
        expired.append(timers_.begin()->second)
        active_.erase(seq)
        timers_.erase(timers_.begin())

    calling_expired_ = true
    for t in expired:
        try: t->run()
        catch: LOG_ERROR
    calling_expired_ = false

    for t in expired:
        if t.repeat and t.seq not in canceling_seqs_:
            t->restart(now)                // expiration_ = now + interval
            timers_.insert({t->expiration_, t})
            active_[t->seq] = t
        else:
            delete t
    canceling_seqs_.clear()

    if timers_.empty(): disarm_timerfd()
    else: reset_timerfd(timers_.begin()->first)
```

### 算法 3：cancel

```cpp
void TimerQueue::cancel(TimerId id):
    it = active_.find(id.seq)
    if it not found:
        if calling_expired_: canceling_seqs_.push(id.seq)
        return
    t = it->second
    was_top = timers_.begin()->second == t
    timers_.erase({t->expiration, t})        // O(log n)
    active_.erase(it)
    if calling_expired_: canceling_seqs_.push(id.seq)
    delete t
    if was_top:
        if timers_.empty(): disarm()
        else: reset_timerfd(new_top)
```

**为什么 cancel 要看 calling_expired_**：在 callback 内 cancel 一个本身已被弹出 expired 列表的 repeating timer，普通的 active_ 查找会 miss。我们记录到 `canceling_seqs_`，让 handle_expired 在重插循环里跳过它。这是 muduo 的同款模式。

### 为什么 std::set + unordered_map 双索引

- `std::set` 按到期排序，`begin()` 拿最近的 O(log n)，按 iterator erase O(log n)
- `unordered_map` 按 seq O(1) 查找
- `priority_queue` 不支持中间删除，故不用
- 用 sequence number 而不是 Timer* 做 cancel key —— Timer 可能 delete 后地址被新 Timer 复用（ABA），seq 单调递增可避免

---

## §三.10　CoroThreadPool：业务线程池（`thread_pool.hpp` / `submit_awaiter.hpp`）

### 数据结构

```cpp
class CoroThreadPool {
    std::vector<std::thread> threads_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
    std::atomic_bool stopping_{false};
};

template <typename R, typename F>
class SubmitAwaiter {
    CoroThreadPool* pool_;
    F fn_;
    std::variant<std::monostate, R_or_monostate, std::exception_ptr> result_;
    std::coroutine_handle<> handle_;
    Scheduler* src_sched_;   // 协程原来在哪个 IO worker 上
};
```

### 算法：跨线程协程恢复

```cpp
// 用户写：auto r = co_await pool.submit([]{ return heavy(); });

template <typename F>
auto CoroThreadPool::submit(F&& fn) {
    using R = std::invoke_result_t<F>;
    return SubmitAwaiter<R, std::decay_t<F>>(this, std::forward<F>(fn));
}

template <typename R, typename F>
void SubmitAwaiter<R,F>::await_suspend(std::coroutine_handle<> h) {
    handle_ = h;
    src_sched_ = Scheduler::current();   // ★ 记住"我在哪个 worker 上挂起"
    pool_->enqueue([this] {
        // 这段在业务线程里跑
        try { result_ = fn_(); }
        catch (...) { result_ = std::current_exception(); }
        src_sched_->post(handle_);       // ★ 让原 worker resume 我
    });
}
```

`src_sched_->post(handle_)` 走的就是 §三.4 的跨线程路径：
- `cross_.handles.push_back(handle_)`
- `write(src_sched_->wake_fd_, 1)`
- 原 worker 的 EventfdWatcher CQE 触发，drain_cross_queue 拿到 handle，push 到 ready_，下一轮 resume

**关键性质**：协程**在原 worker 上 resume**，不在业务线程上。所以后续的 `conn->send` 仍走原 worker 的 ring，fd 没跨线程，io_uring SQ 也无锁。

---

## §三.11　AsyncLogger：muduo 风格双缓冲异步日志（`log/`）

### 数据结构

```cpp
// 前端：每次 LOG_* 构造一个临时 Logger 对象
class Logger {
    LogStream stream_;        // 4000B 栈缓冲 + operator<< 链
    LogLevel level_;
    SourceFile file_;
    int line_;
    ~Logger() noexcept;       // 析构时把整行扔给 AsyncLogger
};

// 后端：单例
class AsyncLogger {
    std::mutex mutex_;
    std::condition_variable cond_;
    std::unique_ptr<LargeBuffer> current_;     // 4MB 当前 buffer
    std::unique_ptr<LargeBuffer> next_;        // 4MB 备份
    std::vector<std::unique_ptr<LargeBuffer>> buffers_;  // 待刷盘队列
    std::thread thread_;
    std::unique_ptr<LogFile> logFile_;         // 滚动文件
};
```

### 前端流程

```cpp
LOG_INFO << "x=" << 42 << " y=" << 3.14;

// 宏展开为：
if (Logger::global_level() <= INFO)
    Logger(__FILE__, __LINE__, INFO).stream() << "x=" << 42 << ...;

// Logger 构造：
//   - 取 TLS 时间字符串（上一秒缓存，跨秒才 localtime_r）
//   - 取 TLS tid
//   - prefix 写到 stream_: "20260526 01:45:10.123456 12345 INFO  "
// stream_ << 各种值：直接 memcpy 进 4000B 栈缓冲
// ~Logger:
//   - 追加 " - basename:line\n"
//   - AsyncLogger::instance().append(stream_.data, stream_.length)
```

`stream_.append(int)` 走 muduo 的 `convert<T>` 双字符表：

```cpp
const char digits[] = "9876543210123456789";
const char* zero = digits + 9;  // 指向 '0'，左右都映射到数字

template <typename T>
size_t convert(char buf[], T value) {
    T i = value;  char* p = buf;
    do {
        int lsd = i % 10;
        i /= 10;
        *p++ = zero[lsd];   // 关键：负数也能正确取数字字符
    } while (i != 0);
    if (value < 0) *p++ = '-';
    std::reverse(buf, p);
    return p - buf;
}
```

比 `snprintf("%d", v)` 快约 8 倍。

### 后端流程

```cpp
void AsyncLogger::append(data, len):
    lock(mutex_)
    if (current_->avail() > len):
        memcpy → current_                // 一般情况
    else:
        buffers_.push_back(move(current_))
        current_ = move(next_)            // 用预先准备的 next_
        memcpy → current_
        cond_.notify_one()                // 唤醒后端
    unlock
```

后端线程：

```cpp
void AsyncLogger::thread_func():
    new_buf1, new_buf2 = 两个空 buffer
    buffers_to_write = []
    while running_:
        {
            unique_lock lk
            if buffers_.empty(): cond_.wait_for(lk, 3s)  // 3秒强制刷
            buffers_.push_back(move(current_))            // 强制把当前 buffer 也吐出
            current_ = move(new_buf1)
            buffers_to_write.swap(buffers_)               // 短临界区
            if !next_: next_ = move(new_buf2)
        }   // 释放锁
        if buffers_to_write.size() > 25:
            // 后端跟不上：丢弃保命
            warn_drop(); truncate to 2
        for buf in buffers_to_write:
            logFile_->append(buf)
        new_buf1 = recycled buffer
        logFile_->flush()
```

### LogFile：滚动落盘

```cpp
class LogFile {
    AppendFile file_;                // 包装 FILE* + 64KB 用户态缓冲
    off_t   roll_size_;              // 默认 64MB
    int     flush_interval_;         // 默认 3s
    time_t  last_roll_;
    int     same_second_seq_;        // 同一秒多次 roll 时区分文件名
};

void LogFile::append(line, len):
    file_.append(line, len)
    if file_.written > roll_size_: roll_file()
    every check_every_n writes:
        if 跨天: roll_file()
        elif 超过 flush_interval_ 秒未刷: file_.flush()
```

文件名：`basename.YYYYMMDD-HHMMSS.host.pid.log`，同秒多次滚动追加 `-1 / -2` 后缀。

---

## §三.12　CMake 构建（`CMakeLists.txt`）

```cmake
cmake_minimum_required(VERSION 3.13)
project(coro_net LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pkg_check_modules(URING REQUIRED liburing)

file(GLOB_RECURSE CORO_NET_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc)
add_library(coro_net SHARED ${CORO_NET_SOURCES})
target_include_directories(coro_net PUBLIC include)
target_link_libraries(coro_net PUBLIC ${URING_LIBRARIES} pthread)

# GCC 的 -fcoroutines 是 GCC-only flag，Clang 14+ 默认支持 C++20 协程不接受这个 flag
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(coro_net PRIVATE -fcoroutines)
endif()
```

GLOB_RECURSE 自动收新 `.cc`，加文件后 `cmake -B build` 重新配置即可。

---

# 第四部分　目录速查

```
coro_net/
├── include/coro_net/
│   ├── task.hpp / fire_and_forget.hpp           协程返回类型
│   ├── io_operation.hpp                         io_uring 操作 awaiter 基类
│   ├── scheduler.hpp / scheduler_pool.hpp       单 worker / 多 worker
│   ├── ops.hpp / ops/*.hpp                      6 个具体 awaiter
│   ├── buffer.hpp / inet_address.hpp            数据 / 地址
│   ├── tcp.hpp（umbrella）/ tcp_connection.hpp / tcp_server.hpp
│   ├── idle_entry.hpp / idle_connection_wheel.hpp
│   ├── timer/timer_id.hpp / timer.hpp / timer_queue.hpp
│   ├── thread_pool.hpp / submit_awaiter.hpp
│   ├── log.hpp / log/*.hpp                      AsyncLogger 全套
│   └── io/io_uring.h                            liburing 薄封装
├── src/
│   ├── coroutine/io_operation.cc / fire_and_forget.cc
│   ├── scheduler/scheduler.cc / scheduler_pool.cc
│   ├── net/tcp_connection.cc / idle_connection_wheel.cc / tcp_server.cc
│   ├── thread_pool/coro_thread_pool.cc
│   ├── timer/timer_queue.cc
│   ├── log/log_stream.cc / logger.cc / log_file.cc / async_logger.cc
│   └── util/circular_buffer.h
├── example/echo_server.cc
└── test/test_*.cc                               9 个 binary 共 31+ 用例
```

---

# 第五部分　最小可工作示例

```cpp
#include "coro_net/tcp.hpp"
#include "coro_net/log.hpp"
#include <chrono>
#include <csignal>

static coro_net::TcpServer* g_server = nullptr;
static void on_signal(int) { if (g_server) g_server->stop(); }

int main() {
    coro_net::init_logger("echo_server");
    LOG_INFO << "starting on :8002";

    coro_net::TcpServer server({8002, "0.0.0.0"}, /*workers=*/4);
    server.set_idle_timeout(std::chrono::seconds(60));

    server.set_handler([](coro_net::TcpConnectionPtr conn) -> coro_net::Task<void> {
        coro_net::Buffer buf;
        while (true) {
            ssize_t n = co_await conn->recv(buf);
            if (n <= 0) break;
            co_await conn->send(buf.retrieveAllAsString());
        }
        co_return;
    });

    g_server = &server;
    std::signal(SIGINT, on_signal);
    server.start();

    // 心跳：每 5 秒打一行日志
    server.pool().at(0).run_every(std::chrono::seconds(5), [] {
        LOG_INFO << "heartbeat";
    });

    server.wait();
    LOG_INFO << "exited";
    coro_net::shutdown_logger();
}
```

读懂这个 example 意味着你已经掌握了 coro_net 的对外 API；读懂 §三对应模块的实现意味着你已经掌握了它的全部内部机制。
