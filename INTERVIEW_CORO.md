# coro_net 简历项目面试问答集

> 三大板块：
>
> **A 组**：技术栈基础与对比（协程 / io_uring / IO 模型 / 同步原语）
> **B 组**：项目组件架构决策（每模块为什么这么设计）
> **C 组**：衍生八股（TCP / 操作系统 / io_uring 内核实现 / 协程底层 / 内存与性能）
>
> 每题答案 150-500 字，可直接面试时复述。

---

# A 组　技术栈基础与对比

## A1. C++20 协程的三大关键字？协程函数和普通函数本质区别？

`co_await` / `co_yield` / `co_return`。编译器看到函数体内任一关键字就把这个函数变成状态机：

1. 调用协程时，先在堆上 `new` 一个"协程帧"（保存局部变量、状态点、promise_type）
2. 立即返回一个由 `promise_type::get_return_object()` 构造的对象（本库的 `Task<T>`）
3. 真正执行由 `std::coroutine_handle` 推进：`.resume()` 恢复、`.destroy()` 销毁

**本质差异**：普通函数的栈在 return 后销毁；协程帧在堆上、由 handle 持有，可在任意时间点 resume —— 这就是协程能"挂起 / 恢复"的根本原因。

## A2. 协程 vs 线程：栈空间、切换成本、调度方式

| | OS 线程 | C++20 协程 |
|---|---|---|
| 栈大小 | 默认 8 MB 虚拟地址 | 协程帧约几百字节，按需在堆上 |
| 切换成本 | 1-3 μs（进内核 + 寄存器 + TLB） | < 100 ns（纯用户态，挂起点处的局部变量保存） |
| 调度方式 | 内核抢占式 | 用户态协作式（必须显式 co_await 才让出） |
| 数量上限 | 万级（受内存限制） | 百万级（受协程帧累积内存限制） |
| 并行性 | 真并行（多核） | 单协程只在一个线程跑，并行靠多个 worker |

**实践含义**：协程不替代线程，**协程是"线程内复用线程的工具"**——一个 OS 线程跑一个调度器，调度器内部跑成千上万协程。本库的 worker 数 = OS 线程数（典型 4-16）。

## A3. 协程 vs 回调：解决了什么，代价是什么？

**回调风格**：

```cpp
sock.async_read(buf, [&](auto err, size_t n) {
    process(buf, n);
    sock.async_write(rsp, [&](auto err) { /* 又一个嵌套 */ });
});
```

问题：状态分散到多个 lambda（要靠捕获 / 结构体维持上下文）、错误传播要每层处理、生命周期复杂。

**协程风格**：

```cpp
size_t n = co_await sock.read(buf);
process(buf, n);
co_await sock.write(rsp);
```

代码线性、像同步、抛异常自然传播。

**代价**：
- 协程帧堆分配（HALO 优化在某些条件下可消除）
- 编译时间增加（状态机展开）
- 调试难度提升（栈不连续，多数 IDE 协程支持仍不完美）
- 学习曲线陡（promise_type / awaiter / symmetric transfer 等概念）

## A4. C++20 协程 vs Go goroutine / Lua / Python async

| | C++20 | Go | Lua | Python async |
|---|---|---|---|---|
| 类型 | stackless | stackful | stackful | stackless |
| 调度器 | 你自己写 | 语言内置 M:N | 你自己写 | asyncio 内置 |
| 切换成本 | 极低 | 极低 | 中（栈拷贝） | 较低 |
| 内存 | ~100B-1KB 帧 | 初始 2KB 栈、动态扩 | 几 KB 栈 | ~600B Task |

- **stackless**（无栈）：协程帧只保存"跨挂起点活着"的变量；不可以从一个普通函数中间 await（必须 awaitable 链一直到顶层协程函数）
- **stackful**（有栈）：每个协程一个完整栈；可以在任意嵌套函数中挂起；切换时整栈保存

C++ 选 stackless 是为零开销（不用为每协程预留栈）；Go 选 stackful 是为编程便利（用户不感知协程边界）。

## A5. io_uring vs select / poll / epoll

| | select | poll | epoll | io_uring |
|---|---|---|---|---|
| 每次告诉内核多少 fd | 全部（拷贝） | 全部（拷贝） | 增量注册 | 增量注册 |
| 触发 IO 是否额外 syscall | 是 | 是 | 是 | **否**（提交 IO 在 SQ 共享内存里） |
| 一次 wait 复杂度 | O(N) 内核扫 | O(N) 内核扫 | O(就绪数) | O(就绪数) |
| 数据交付 | 通知"就绪" | 通知"就绪" | 通知"就绪" | **直接交付结果** |
| 适用场景 | 老代码 / 极简 | 同上 | 通用网络 IO | 通用 + 磁盘 IO |

**最大差异**：epoll 是"事件就绪通知"，用户拿到 fd 后**还要发 read/write syscall**；io_uring 是"我帮你做完了"，直接拿 CQE 即可。10 万 IO 在 epoll 下要 20 万 syscall（wait + read），在 io_uring 下可压到 1000 次（batch=100）。

## A6. io_uring vs Linux AIO (libaio)

`libaio` 是 Linux 2.6 引入的异步 IO，长期被诟病：
- 只支持 direct IO（O_DIRECT），无法用于 buffered IO（page cache）
- 不支持网络 IO（只支持磁盘）
- API 设计不一致，bug 多
- 实际很多情况下退化为同步执行

io_uring 全面取代 libaio：
- 网络 + 磁盘 + 定时器 + 文件系统操作（open/close/stat）+ 跨 ring 消息全部支持
- buffered IO 也走真异步路径
- 用户/内核共享内存，没有 syscall 进 / 退的开销

## A7. io_uring 关键 setup flags

| flag | 作用 | 代价 |
|---|---|---|
| `IORING_SETUP_COOP_TASKRUN` (5.18+) | 完成事件不主动中断用户态线程，等线程下次进入内核时批处理 | 降低 IRQ 开销，几乎免费；本库默认开 |
| `IORING_SETUP_SQPOLL` | 内核起 kthread 轮询 SQ tail，省 `io_uring_enter` syscall | 占一个 CPU 核 100% busy poll；权限要求 |
| `IORING_SETUP_SINGLE_ISSUER` (6.0+) | 声明"提交 SQE 的线程固定"，内核省 ring 内部同步 | 违反约束返回 -EEXIST；本库构造在 main、run 在 worker，违反约束所以不开 |
| `IORING_SETUP_DEFER_TASKRUN` (6.1+) | 任务推迟到 `io_uring_enter` 时执行，进一步降中断 | 必须配 SINGLE_ISSUER |

## A8. multishot / provide-buffers / registered files 分别是什么？

- **multishot**（accept 5.19+ / recv 5.20+）：一次提交一个 SQE，内核持续产生多个 CQE。`flags & IORING_CQE_F_MORE` 表示"还会继续产生"。收益：长连接 accept/recv 无需每次重新提交 SQE。
- **provide-buffers (PBUF_RING)**：把 buffer 池预注册给内核；recv 时不指定 buffer，内核挑空闲槽位写入，CQE 用 `flags >> IORING_CQE_BUFFER_SHIFT` 携带 buffer_id。收益：N 个空闲连接共享 M 个 buffer，内存 O(M)。**本库已废除**（详见 B3）。
- **registered files (REGISTER_FILES)**：把 fd 预注册成"槽位号"，后续提交时用槽位号代替 fd，省内核 fd 表查找和 refcount 操作。收益：短连接 ~10-20%，长连接 ~5%。本库未启用（管理复杂度高）。

## A9. POSIX 五种 IO 模型 / Reactor vs Proactor

POSIX 五种：
1. **阻塞 IO**：`read` 阻塞到数据来
2. **非阻塞 IO**：`read` 立刻返回，没数据返回 EAGAIN，需要轮询
3. **IO 多路复用**：`select/poll/epoll` 一次问内核多个 fd
4. **信号驱动 IO**：数据到达时 SIGIO，回调通知（很少用）
5. **异步 IO（真正的）**：内核完成 IO 后通知应用，数据已经在 buffer 里

**Reactor**（反应器，epoll 模型）：通知"事件就绪"，用户做 IO。**回调式**：`onReadable() { read(); process(); }`。

**Proactor**（前摄器，io_uring / Windows IOCP 模型）：通知"IO 已完成"，结果就在 buffer 里。**真异步**：`onCompleted(result) { process(); }`。

io_uring 是 Linux 上第一个真正的 Proactor 实现。本库就是 Proactor 模型 + 协程外壳。

## A10. 同步 / 异步 / 阻塞 / 非阻塞 四个词的精确语义

四个词描述两个独立维度：

|  | 阻塞 | 非阻塞 |
|---|---|---|
| **同步**（用户做 IO） | `read()` 阻塞到数据来 | `read()` 立刻返回 EAGAIN，需轮询 |
| **异步**（内核做 IO） | （罕见）AIO with O_SYNC 等 | `io_uring_prep_read` 提交后立刻返回，内核完成后通知 |

- **同步 vs 异步**：谁实际拷数据。同步=用户线程拷；异步=内核拷完通知。
- **阻塞 vs 非阻塞**：发起 IO 的那次调用是否立即返回。

io_uring 是**异步 + 非阻塞**：用户提交 IO 立刻返回（非阻塞），内核完成时把数据放好后通知（异步）。

---

# B 组　项目组件架构决策

## B1. 为什么选每线程一个 io_uring（模型 A）？

三种可选模型：
- **A**：每线程一个 ring，N worker
- **B**：单 ring + SQPOLL，所有线程共享
- **C**：M:N 工作窃取，协程跨线程迁移

**A 优势**：
- SQ/CQ 完全无锁（SINGLE_ISSUER 友好，虽然本库没开）
- 连接绑定 worker，fd 不跨线程，io_uring 不跨线程
- 协程在原 worker 上 resume，无线程迁移成本

**A 劣势**：worker 负载可能不均（round-robin 退化），可通过 least-loaded 派发缓解。

**B 缺点**：SQ/CQ 多线程访问需要 CAS / atomic，可扩展性差。

**C 缺点**：协程跨线程迁移管理 TLS、fd 归属、ring 切换是深坑。C++ 缺 Rust tokio 那种 `Send/Sync` 语言级保证，工程难度大。

业界主流（Photon、seastar、libcoro）都选 A，可扩展性、性能、复杂度的最佳平衡。

## B2. 跨线程唤醒：MSG_RING vs eventfd，本库为什么选 eventfd？

**MSG_RING (5.18+)**：源线程提交 `OP_MSG_RING` SQE 到自己 ring，内核投递一个 CQE 到目标 ring。零 syscall（源端只走 submit），无用户态共享数据结构。

**eventfd**：用户写 8B 到 eventfd，目标 ring 上挂的 `read(eventfd, 8B)` SQE 收到 CQE 醒来。每次唤醒一次 `write` syscall。

**初看 MSG_RING 更优**——零 syscall。但本库最终选 eventfd：

1. **路径单一**：MSG_RING 只适用于"调用线程也有 ring"的情况；无 ring 线程（main、业务线程池 worker）仍要 eventfd 兜底。两条路径并存让 CQE 处理多一种特殊分支
2. **与 shared-nothing 一致**：原方案区分"有 ring 走 MSG_RING、无 ring 走 eventfd"实质把跨线程通信分成两类；统一后所有跨线程通信走同一窄路
3. **代价小**：跨线程唤醒不是热点（一秒几千次），多一次 `write(eventfd)` 纳秒级，可忽略

**面试要点**：能讲清"原方案的优势 + 为什么选/弃"，体现技术决策不是单向、要看上下文。

## B3. Buffer 设计：为什么沿用 muduo 风格而非用 io_uring BufferRing？

**muduo 风格 Buffer**：`vector<char>` + 三段式（prependable / readable / writable）+ kCheapPrepend 8B 保留区。

**BufferRing 方案**（早期）：每 worker 一个 4MB ring buffer（1024 × 4KB），recv 用 `IOSQE_BUFFER_SELECT`，内核挑槽位写，CQE 携带 buffer_id，用户 memcpy 到 per-conn Buffer 再归还槽位。

**改回 per-conn 直写**的理由：
1. **少一次 memcpy（~200ns/4KB）**：io_uring 直接把数据写到 `buf.beginWrite()`，CQE 来时 `buf.hasWritten(n)`，零拷贝
2. **代码简化**：删 BufferRing 类、IORING_CQE_F_BUFFER 解析、IOSQE_BUFFER_SELECT 配置
3. **错误码语义回归 POSIX**：不再有 `-ENOBUFS`

**代价**：内存按连接独立持有，10K 连接 ≈ 10MB/worker（vs 固定 4MB）。本项目偏 RPC，可接受；海量空闲连接场景（IM、推送）BufferRing 仍占优。

## B4. 时间轮清除空闲连接：怎么做到 O(1)？

**朴素方案**：每连接记 `last_active_time`，定时扫描所有连接 → O(N) 每秒。

**时间轮方案（本库）**：
- `CircularBuffer<unordered_set<shared_ptr<IdleEntry>>>` 大小 = idle_seconds（典型 60）
- 每个连接持有一个 `shared_ptr<IdleEntry>`，只放在"队尾桶"
- 1Hz 滴答：`buckets_.push_back(empty)`，覆盖最旧桶 → 旧桶里 entry 引用计数 -1，归零则 `~IdleEntry` 触发关闭
- 续命：连接有数据来时，把它的 `shared_ptr<IdleEntry>` 重新插队尾桶 → 引用计数 +1

**复杂度**：
- 滴答 O(1)（一次 push_back + 一个桶批量析构）
- 续命 O(1)（哈希集合插入）
- **完全与连接数无关**

**为什么用 shared_ptr 引用计数判断淘汰**：避免单独维护"还存在于哪些桶"的元数据，让 C++ RAII 自动处理。当 entry 不再被任何桶持有，析构自然触发关闭逻辑。

**坑**：`make_shared<IdleEntry>(IdleEntry{c, sched})` 中的临时对象在 make_shared 内部 move 后被析构，会**误触发**关闭。修复：用显式构造 `make_shared<IdleEntry>(c, sched)` 直接堆上构造。

## B5. 通用 TimerQueue 为什么 per-Scheduler 而非全局堆？

**方案**：每个 Scheduler 自己的 TimerQueue —— 一个 timerfd + `std::set<{expire, Timer*}>` 排序堆 + `unordered_map<seq, Timer*>` 序列号查表。

**不全局堆的原因**：
1. **与 shared-nothing 一致**：io_uring / Buffer / ready 队列全部 per-worker，timer 也应该
2. **timerfd 必须挂在某个 ring 上**：那个 ring 是哪个 worker 的，timer 就属于哪个 worker。全局堆要么单独跑一个 ring（多一个线程），要么 epoll(timerfd) 异构唤醒（割裂）
3. **避免锁竞争**：全局堆所有插入/取消都竞争一把锁；per-worker 操作纯本地、cache 热

**为什么 std::set 而非 priority_queue**：
- `std::set` 按到期排序，O(log n) 插入；按 iterator erase O(log n) → 支持 cancel
- `priority_queue` 不支持中间删除

**为什么需要 unordered_map 辅助索引**：cancel 用 sequence 查 O(1)，再用得到的 Timer* 从 std::set erase O(log n)；如果只用 set 找元素就要 O(n)。

**TimerId 用 sequence 防 ABA**：Timer 析构后地址可能被新 Timer 复用，序列号单调递增可避免误删。

**回调内 cancel**：`calling_expired_` 标记 + `canceling_seqs_` vector —— 在 callback 内 cancel 一个 *尚未* 被重插的 repeating timer 时，把序列号记下来，handle_expired 在重插循环里跳过它。muduo 同款模式。

## B6. AsyncLogger 双缓冲为什么这样设计？

**约束**：
- 不引入第三方依赖
- 前端必须几乎零成本（IO worker 上跑日志不能阻塞）
- 必须支持流式语法 `LOG_INFO << ...`

**设计要点**：

1. **前端临时 Logger 对象**：`LOG_INFO` 宏展开为 `Logger(__FILE__, __LINE__, INFO).stream()`。Logger 构造写时间/tid/level 前缀，析构追加换行+源位置，把整行交给 backend
2. **TLS 时间字符串缓存**：跨秒才调一次 `localtime_r`，同秒直接 memcpy 上次结果。日志高峰只 +1 syscall/秒
3. **整型 muduo 双字符表**：自实现 `convert<T>` 比 `snprintf` 快 8 倍
4. **双 4MB buffer**：`current_` / `next_`；前端持锁 memcpy 进 `current_`，满则换 `next_` 并通知后端
5. **后端独立线程**：`cv.wait_for(3s)`，超时或被唤醒后 swap 出 `buffers_`、**释放锁**再写盘。临界区只有 vector swap 和 memcpy，极短
6. **背压保护**：buffers_ 积压 > 25 个（100MB 未刷）→ 丢弃多余 + 写一行警告

**编译期 + 运行期双层过滤**：
- 编译期：`CORO_NET_LOG_MIN_LEVEL_VAL`，低于此级别的 `LOG_TRACE/DEBUG` 整段预处理掉
- 运行期：`Logger::set_global_level()` 切换，线上调试用

**LOG_FATAL 特殊路径**：`~Logger` 同步调 `AsyncLogger::flush_and_stop()`（join backend + 刷完最后一行）再 `abort()`，保留 coredump 时日志完整。

## B7. CoroThreadPool 怎么实现"业务跑独立线程、协程在原 worker resume"？

```cpp
co_await pool.submit([]{ return heavy(); });
```

展开：
1. `submit(F)` 返回 `SubmitAwaiter<R, F>`，`co_await` 触发 `await_suspend(h)`
2. `await_suspend`:
   - 记下 `src_sched_ = Scheduler::current()` ★（"我在哪个 worker 上挂起"）
   - 记下 `handle_ = h`
   - 把 lambda 入队到 pool：`pool->enqueue([this] { result_ = fn_(); src_sched_->post(handle_); })`
3. 业务线程取出 lambda → 执行 → 把结果写到 `awaiter->result_` → 调 `src_sched_->post(handle_)`
4. `src_sched->post`：业务线程 ≠ src_sched 线程 → 走 `wake_remote()` 路径 → `write(eventfd, 1)`
5. src_sched 的 EventfdWatcher CQE 到达 → drain_cross_queue 把 handle 推 ready_ → 下一轮 resume
6. `await_resume` 读 `result_` 返回值给协程

**关键性质**：协程**在原 worker 上 resume**，不在业务线程。后续 `conn->send` 仍走原 worker 的 ring，fd 没跨线程使用，io_uring SQ 也无锁。

**为什么这样设计**：让 fd 与 worker 线程亲和不被打破；业务线程只做 CPU 工作不碰 IO。

## B8. 优雅停机怎么做？

`TcpServer::stop()` 链：
1. `running_.store(false)` 原子置位
2. `shutdown(listen_fd, SHUT_RDWR)` —— 让 `AcceptAwaiter` 的 CQE 收到 `-ECANCELED` 自然退出
3. 对每个 worker：`post_task(sched, [&]{ sched.stop(); })` —— 跨线程派发（main 无 ring，走 eventfd）
4. 每个 worker 在主循环顶部检测 `stopping_`，跑完当前 ready 后退出
5. `pool.wait()` join 所有 worker 线程

**在途连接**：listen_fd 关闭后无新连接；现有 recv 协程在某轮可能返回 `-ECANCELED` 或 0，handler 自然 `co_return`，协程帧释放，`shared_ptr<TcpConnection>` 引用计数归零，`~TcpConnection` 关闭 fd。

**弱点**：当前没显式 cancel 在途 IO，依赖 fd 关闭让内核自然取消。生产化可以提交 `IORING_OP_ASYNC_CANCEL` 主动取消。

## B9. Task 和 FireAndForget 为什么并存？

| | Task<T> | FireAndForget |
|---|---|---|
| initial_suspend | suspend_always (lazy) | suspend_never (eager) |
| final_suspend | FinalAwaiter（symmetric transfer 跳回外层） | suspend_never（自动销毁） |
| 谁来 await | 必须有外层 co_await | 没人 await |
| 用途 | 业务协程逻辑（recv/send/RPC handler） | 顶层 spawn 入口（accept_loop / timer 滴答） |

**Task 是 lazy 的好处**：
- 语义清晰："承诺"和"兑现"分离
- 配合 symmetric transfer，深层 await 不爆栈

**FireAndForget 是 eager 的必要**：
- Scheduler::spawn 启动的 handler 协程没人 await，需要立即执行 + 结束自动销毁
- 实现：`Scheduler::spawn(Task<void>)` 内部 `spawn_shim` 包一层 FireAndForget

---

# C 组　衍生八股

## C1. TCP 三次握手 / 四次挥手

**三次握手**（建立）：
1. 客户端 SYN（seq=x）
2. 服务端 SYN+ACK（seq=y, ack=x+1）
3. 客户端 ACK（seq=x+1, ack=y+1）

**为什么三次**：两次不够—— 服务端无法确认客户端的接收能力（也即"我能收到客户端的 ACK"）；四次冗余（第二步 SYN 和 ACK 可合并为一个包）。

**四次挥手**（关闭）：
1. 主动方 FIN
2. 被动方 ACK
3. 被动方 FIN（处理完剩余数据才发）
4. 主动方 ACK，进入 TIME_WAIT 2*MSL 后关闭

**为什么四次**：TCP 全双工，关闭要分别确认两个方向。第 2 步和第 3 步不能合并是因为被动方收到 FIN 后可能还有数据要发。

## C2. TIME_WAIT 和 CLOSE_WAIT 怎么处理？

**TIME_WAIT**：主动关闭方处于此状态 2*MSL（典型 60s）。目的：
1. 让"晚到的报文"消亡（避免被新连接误收）
2. 确保对端能收到最后的 ACK（如果对端没收到会重传 FIN）

**问题**：大量 TIME_WAIT 占用端口资源（典型 4 元组冲突时无法发起新连接）。

**解决方案**：
- `SO_REUSEADDR`：允许重用处于 TIME_WAIT 的端口（服务端常用，重启后不用等）
- 调小 `net.ipv4.tcp_fin_timeout`
- 启用 `tcp_tw_reuse`（5.x 内核默认 0，要 1 才允许在客户端重用 TIME_WAIT）
- 长连接 / 连接池减少建连频率

**CLOSE_WAIT**：被动关闭方处于此状态，等待应用调用 `close()`。**大量 CLOSE_WAIT 是 bug**——应用漏调用 close。本库的 handler `co_return` 会析构 `shared_ptr<TcpConnection>`，`~TcpConnection` 调 `close(fd)`，正常情况不会堆积。

## C3. SO_REUSEADDR vs SO_REUSEPORT

| | SO_REUSEADDR | SO_REUSEPORT (Linux 3.9+) |
|---|---|---|
| 用途 | 允许 bind 同地址+不同端口、或重用 TIME_WAIT 状态的地址 | 多个 socket bind 同一地址+端口，内核负载均衡 |
| 用法 | 服务端重启不用等 TIME_WAIT 消失 | 多 listener accept 时内核哈希派发，省单 acceptor 瓶颈 |
| 安全性 | 较安全 | 较安全（5.0+ 加了进程 UID 校验防劫持） |

本库 `tcp_server.cc:make_listen_socket` 同时设了两个，方便重启。

## C4. TCP 半包粘包是什么？怎么解决？

**根源**：TCP 是字节流协议，没有"消息边界"。发送方 `send("ABC")` + `send("DEF")` 接收方可能 `recv` 一次拿到 "ABCDEF"（粘包），也可能 `recv` 两次各拿一半"AB"+"CDEF"（半包）。

**根本原因**：
- Nagle 算法把小包合并
- 接收方 socket buffer 一次性吐多个 send 的数据
- MTU 限制让大包拆分

**三种通用解决方案**：

1. **定长**：每个消息固定长度（如 1024B），不足补零。简单但浪费。
2. **分隔符**：用特殊字符（如 `\r\n`）分包。HTTP 报文头用这个。需要转义。
3. **长度前缀**（推荐）：每个消息前 4B 长度。本库 Buffer 的 `kCheapPrepend` 8B 保留区就是为这设计的。

**典型拆帧循环**：

```cpp
Task<void> handler(TcpConnectionPtr conn) {
    Buffer buf;
    while (true) {
        ssize_t n = co_await conn->recv(buf);
        if (n <= 0) break;
        while (buf.readableBytes() >= 4) {
            uint32_t len;
            std::memcpy(&len, buf.peek(), 4);
            len = ntohl(len);
            if (buf.readableBytes() < 4 + len) break;   // 半包，等下次
            std::string body(buf.peek() + 4, len);
            buf.retrieve(4 + len);                       // 消费
            process(body);
        }
    }
}
```

## C5. Nagle 算法和 TCP 延迟确认

**Nagle**：发送方不立即发小包，攒到 MSS 大小或上一个 ACK 回来才发。**作用**：避免大量小包（telnet 一个字符一个包就 100% IP 头开销）。**问题**：每次至少多一个 RTT 延迟。

**延迟 ACK**（Delayed ACK）：接收方不立即回 ACK，攒 40ms 或攒到要回数据时一起发。**作用**：减少 ACK 包数量。**问题**：和 Nagle 配合时会出现"双方都在等"的 200ms 大延迟。

**解决方案**：
- 低延迟应用关 Nagle：`setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one)`
- 或攒一个完整请求再 send（让 send 量本来就 ≥ MSS）

本库 send 单次写所有数据，加上 TCP_NODELAY 即可避免 Nagle 影响。

## C6. TCP 拥塞控制 / 流量控制

**流量控制（接收方驱动）**：接收方在 ACK 里告诉发送方"我的 buffer 还能收多少"（rwnd）。发送方不超 rwnd 即可。

**拥塞控制（网络驱动）**：基于丢包 / RTT 推测网络拥塞，主动降速。经典算法：
- **慢启动**：cwnd 从 1 开始指数增长，到 ssthresh 切换为线性增长
- **拥塞避免**：cwnd 线性 +1/RTT
- **快速重传**：收到 3 个重复 ACK 立即重传，无需等超时
- **快速恢复**：重传后 cwnd 减半（不退回慢启动）

现代默认算法：CUBIC（Linux 默认）、BBR（Google 提出，主动测带宽和 RTT，对长肥管道效果好）。

## C7. epoll LT 和 ET 模式区别

- **LT (Level Triggered)**：只要 fd 处于就绪状态，`epoll_wait` 就会一直返回它。简单但可能多次 wakeup。
- **ET (Edge Triggered)**：fd 状态变化才返回一次（从未就绪 → 就绪）。需要应用一次性把数据读完（循环 read 到 EAGAIN），否则下次 epoll_wait 不会再通知。

**ET 的优势**：减少 epoll_wait 唤醒次数，性能略高。

**ET 的代价**：编程复杂，必须配合非阻塞 fd + 循环读取直到 EAGAIN。

io_uring 不存在 LT/ET 区分——它是"完成通知"而非"就绪通知"。

## C8. 进程 / 线程 / 协程 区别

| | 进程 | 线程 | 协程 |
|---|---|---|---|
| 地址空间 | 独立 | 共享 | 共享 |
| 调度 | 内核 | 内核 | 用户态 |
| 切换成本 | 几 μs（含 TLB 刷新） | 1-3 μs | < 100 ns |
| 通信 | IPC（管道、共享内存等） | 锁 + 共享变量 | 直接共享 |
| 创建开销 | 几 ms（fork） | 几十 μs | 几百 ns |
| 数量 | 千级 | 万级 | 百万级 |

**协程的本质**：用户态实现的"轻量线程"，跑在某个真正的 OS 线程内。多个协程在同一线程里**协作式**调度（必须显式 yield/await 让出），不会被抢占。

## C9. 用户态 / 内核态、syscall 怎么进入内核？

**用户态 vs 内核态**：CPU 有特权级别（x86 的 ring 0/3），内核态可以访问任意内存和硬件，用户态被限制。

**syscall 流程**（x86_64）：
1. 把系统调用号放 `rax`，参数放 `rdi/rsi/rdx/r10/r8/r9`
2. 执行 `syscall` 指令：CPU 切换到 ring 0，跳转到内核预设的入口（`entry_SYSCALL_64`）
3. 内核根据 `rax` 查 `sys_call_table[]` 找到对应函数
4. 执行后 `sysret` 切回 ring 3，结果放 `rax`

**开销**：纯切换约 50-100ns。还要考虑 CPU pipeline flush、SMAP（Supervisor Mode Access Prevention）、KPTI（Meltdown 缓解）等额外开销。

**减少 syscall** 是性能优化关键。io_uring 共享内存就是为此。

## C10. mmap 共享内存怎么工作？io_uring 的 SQ/CQ 怎么用？

`mmap(addr, len, prot, flags, fd, offset)` 把文件 / 匿名内存映射到进程虚拟地址空间。两种用法：
- **文件映射**：把磁盘文件映射到内存，read/write 变成直接读写内存（OS 帮你 paging）
- **匿名映射**：纯内存（fd=-1, MAP_ANONYMOUS），用于 malloc / 进程间共享

**io_uring 的 SQ/CQ**：内核分配一段内核内存存 SQE/CQE 数组，然后 mmap 给用户进程。同一段物理页同时被内核和用户访问。

```
   用户虚拟地址                                  内核虚拟地址
   ┌──────────┐                              ┌──────────┐
   │  SQ ring │  ──── 同一段物理内存 ────►   │  SQ ring │
   │  (mmap)  │                              │ (kernel) │
   └──────────┘                              └──────────┘
```

用户写 SQE 就是写这段内存；内核读 SQE 也是读这段内存。**没有数据拷贝**。同步靠 head/tail 原子下标 + memory_order_release/acquire 内存序。

## C11. 协程帧的内存布局怎么决定？stackless vs stackful？

**stackless 协程**（C++20、Python async、Rust async）：
- 编译器分析"跨挂起点活着"的变量，把它们打包成一个 struct 存到协程帧
- 帧 = `promise_type` + 局部变量 + 状态点编号 + resume/destroy 函数指针
- 内存只够装这些跨挂起点变量 → 极省内存（百字节级）
- 限制：不能在普通函数中间挂起（只能在协程函数的 await 表达式处）

**stackful 协程**（Go goroutine、Lua coroutine、boost.context）：
- 每个协程有完整独立的栈（可能 2KB-64KB 起步）
- 切换时整个栈帧保存（其实只保存 SP、PC、几个寄存器）
- 可以从任意嵌套函数中挂起
- 内存开销较大、栈溢出问题

**HALO（Heap Allocation eLision Optimization）**：C++20 协程的特殊优化——若编译器能证明协程帧生命周期不超出调用者（inline + 直接 co_await），可以把帧分配到调用者的栈上，零堆分配。GCC 13 / Clang 17+ 支持，条件苛刻。

## C12. co_await 表达式编译器怎么展开？

`co_await expr` 在编译器内部大约展开为：

```cpp
{
    // 1. 把 expr 转成 awaiter（通过 operator co_await 或直接是 awaiter）
    auto&& awaiter = ...;

    // 2. 询问能否免去挂起
    if (!awaiter.await_ready()) {
        // 3. 把协程状态保存到协程帧
        __builtin_coro_save();

        // 4. 决定挂起方式
        using R = decltype(awaiter.await_suspend(handle));
        if constexpr (std::is_same_v<R, void>) {
            awaiter.await_suspend(handle);
            // 跳回调用方
        } else if constexpr (std::is_same_v<R, bool>) {
            if (awaiter.await_suspend(handle)) {
                // 跳回调用方
            }
            // 否则继续（不挂起）
        } else {
            // R 是 coroutine_handle<> → symmetric transfer
            auto next = awaiter.await_suspend(handle);
            __builtin_coro_resume(next);  // 编译为 jmp
        }

        // 5. resume 后从这里继续
        __builtin_coro_restore();
    }

    // 6. 取结果
    expr_value = awaiter.await_resume();
}
```

整个协程函数被切成多段，每段对应一个 "状态点"，由状态点编号驱动跳转。Resume 函数大约是 `switch(state) { case 0: ...; case 1: ...; }`。

## C13. 内存序：acquire / release / seq_cst 是什么？

C++11 引入 atomic 内存序，描述对其他线程可见性：

- **relaxed**：只保证原子性，不保证顺序。计数器加减用。
- **acquire**：本操作之前看到的修改，对后续 read 可见。读者用。
- **release**：本操作之后的修改，对其他 acquire 同一变量的线程可见。写者用。
- **acq_rel**：兼具 acquire 和 release。读改写操作用。
- **seq_cst**：最严格，所有线程看到的所有 seq_cst 操作有一致的全局顺序。默认。

**典型 producer-consumer pattern**：
```cpp
// Producer
data = 42;
ready.store(true, std::memory_order_release);

// Consumer
while (!ready.load(std::memory_order_acquire)) {}
assert(data == 42);   // ★ 由 release/acquire 配对保证可见
```

io_uring 的 SQ/CQ 同步就用 release/acquire 配对：内核 release-store SQ head（"我处理到这了"），用户 acquire-load 知道哪些 SQE 已被消费。

## C14. cache line / false sharing / NUMA

**cache line**：CPU cache 的最小单位，典型 64B。访问内存时整个 cache line 被加载。

**false sharing**：两个无关变量碰巧落在同一个 cache line，多核分别写各自的变量，但 cache coherence 协议（MESI）会让两个核反复 invalidate 对方的 cache line，性能暴跌。

**典型场景**：

```cpp
struct Counter { int a; int b; };  // a 和 b 在同一 cache line
// Core 0 写 a，Core 1 写 b
// 即使 a/b 互不相关，MESI 协议导致 cache line 在两个核之间反复传递
```

**解决**：`alignas(64)` 把热点变量独占一个 cache line。

**NUMA**（Non-Uniform Memory Access）：多 socket 服务器每个 CPU 有"本地内存"，访问本地内存比远程 socket 内存快 1.5-2 倍。优化：把线程绑定到 CPU（`pthread_setaffinity_np`），让线程访问的内存在本 NUMA node 上。

## C15. 零拷贝技术：mmap / sendfile / splice / io_uring_register_buffers

**传统读文件并发送到 socket**：

```
read(file, buf): file → kernel page cache → user buf  (1 次 copy)
write(sock, buf): user buf → kernel socket buf        (1 次 copy)
```

2 次内存拷贝 + 2 次 syscall。

**零拷贝方案**：
- **sendfile(out_fd, in_fd, ...)**: kernel page cache → kernel socket buf，无用户态参与。1 次 copy + 1 次 syscall。
- **splice + pipe**：通过管道在内核中传递数据，连 cache → socket buf 这次拷贝也省（用 page reference）
- **mmap + write**：read 改为 mmap，再 write。理论 1 次 copy，但 mmap 有 page fault 成本
- **io_uring_register_buffers**：把用户 buffer 预注册给内核，内核 DMA 直接读写注册的物理页，省 pinning 开销

实际中**最常用的是 sendfile**（Nginx 默认）。**io_uring** 也支持 `OP_SENDFILE`。

## C16. mutex / spinlock / cv 的选择

- **mutex**：阻塞，竞争时进入内核等待（futex）。适合临界区较长（> 1μs）。
- **spinlock**：忙等，不进内核。适合临界区极短（< 1μs，比如几个原子操作）。
- **condition_variable**：基于 mutex，"等条件成立"，被 notify 唤醒。

**典型陷阱**：
- 持锁时调可能阻塞的操作（IO、syscall）→ 锁竞争加剧
- spinlock 持锁时被抢占调度 → 其他线程空转烧 CPU
- cv 不用 while loop 检查条件 → 虚假唤醒导致 race

本库 AsyncLogger 用 mutex + cv：临界区只有 buffer swap，约几十纳秒，但因为 cv 等待会阻塞，整体走 mutex（不是 spinlock）。

## C17. asan / tsan / ubsan / valgrind 怎么用？

- **asan (AddressSanitizer)**：找内存错误（UAF、heap buffer overflow、stack overflow）。`-fsanitize=address -g`。运行时 2-3 倍开销，内存 2-3 倍。
- **tsan (ThreadSanitizer)**：找数据竞争。`-fsanitize=thread`。运行时 5-15 倍开销。
- **ubsan (UndefinedBehaviorSanitizer)**：找未定义行为（整数溢出、空指针 deref 等）。`-fsanitize=undefined`。开销很小。
- **valgrind**：检测内存问题，比 asan 更全但慢 20-50 倍。不需要重新编译。

**本项目实战**：S6 阶段集成测试时 asan 报 UAF——`post_task` 的 lambda 内拷贝了 `handler` std::function，lambda 退出时这个副本析构，但协程帧仍引用它的内部状态。修复：lambda 内只捕获 `TcpServer*`，每次通过 `srv->handler_` 访问原始 closure。

**协程相关的坑用 asan 特别有效**：栈对象引用被协程帧捕获、临时 awaiter 生命周期问题、lambda-coroutine 的 closure 寿命。

## C18. perf / strace / gdb 在协程项目里的使用

- **strace -c -p <pid>**：统计进程的 syscall 频率、耗时。本库优化时验证"io_uring 减少了 epoll 的两次 syscall"就用这个看。
- **perf top -p <pid>**：实时看 hot function。协程项目里能看到 `__resume_<协程函数>` 出现得多说明协程调度频繁。
- **perf record -g + perf report**：生成火焰图。看协程恢复路径上的开销分布。
- **gdb**：调试协程比调试普通函数难得多——栈不连续。需要打开 GCC 的 `-fcoroutines-debug-info`，配合 gdb 的 `info coroutines` 命令（实验性）。本库实测主要靠 LOG_DEBUG 加日志定位。

---

## 附录　项目自我介绍话术（30 秒电梯演讲）

> 我做了一个**底层异步网络库** coro_net，基于 **C++20 协程 + Linux io_uring + 线程池** 实现。库用 SchedulerPool 管理 N 个 worker，**每个独占一个 io_uring 实例 + timerfd + eventfd**，跨线程唤醒统一走 eventfd 保持 shared-nothing 一致性。recv 让 io_uring 直接写到 per-connection Buffer，零拷贝；CPU 工作交给独立的 CoroThreadPool，协程跑完后在原 IO worker 上 resume，整个数据路径不跨线程同步。库内置了**通用 TimerQueue**（per-worker timerfd + std::set 排序堆 + sequence 防 ABA）、**专用空闲连接淘汰时间轮**（CircularBuffer + shared_ptr 引用计数 O(1) 淘汰）、**muduo 风格双缓冲异步 Logger**（前端 LOG_INFO << ... 流式语法，TLS 缓存时间字符串，后端独立线程刷盘），完整覆盖网络库标配。

> 这个项目对我最大的收获是把"看起来像同步的协程代码"和"零开销的异步 IO"绑在一起；同时经历了真实的设计演进——初版用 BufferRing 和 MSG_RING 追极致性能，后来意识到一致性和可读性比这点性能重要，主动砍掉换上更简单的 per-conn 直写 + 统一 eventfd。能讲清楚"为什么选 X"也能讲清楚"为什么后来改回 Y"，体现技术决策不是单向的。途中也踩了协程帧寿命、lambda closure 析构、timerfd 同秒 roll 冲突、SINGLE_ISSUER 约束等坑，用 asan 抓 use-after-free 是关键定位手段。
