// =============================================================================
// benchmark/echo_client.cc —— coro_net 压测客户端
// =============================================================================
//
// 多线程 + epoll-LT + 长连接 echo 压测器。
//
// 设计要点：
//   - 每个线程独立 epoll 实例（无锁）
//   - 每个连接 send→recv 一来一回，固定消息大小
//   - 每个 RTT（微秒）记到 per-thread 数组（避免锁）
//   - 结束时合并所有线程的数组、排序、计算百分位
//
// 用法：
//   ./echo_client --host 127.0.0.1 --port 8002 \
//                 --threads 4 --connections 100 \
//                 --duration 30 --msg-size 64 \
//                 [--warmup 5] [--max-samples 1000000]
//
// 输出（stdout）：
//   THREADS=4 CONN=100 MSG=64B DUR=30s
//   total_messages=NNN  qps=NNN  bytes_throughput=NNN MB/s
//   latency_us: p50=NN p90=NN p99=NN p999=NN min=NN max=NN
//   errors: connect=NN read=NN write=NN timeout=NN
// =============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// -----------------------------------------------------------------------------
// 命令行参数
// -----------------------------------------------------------------------------
struct Options {
    std::string host         = "127.0.0.1";
    uint16_t    port         = 8002;
    int         threads      = 1;
    int         connections  = 100;        // 总连接数；尽量均分到各线程
    int         duration_s   = 30;
    int         warmup_s     = 5;
    int         msg_size     = 64;
    int         max_samples  = 1'000'000;  // 每线程采样数上限；超过随机替换避免内存爆
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s expects an argument\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") o.host = next();
        else if (a == "--port") o.port = (uint16_t)std::atoi(next());
        else if (a == "--threads" || a == "-t") o.threads = std::atoi(next());
        else if (a == "--connections" || a == "-c") o.connections = std::atoi(next());
        else if (a == "--duration" || a == "-d") o.duration_s = std::atoi(next());
        else if (a == "--warmup") o.warmup_s = std::atoi(next());
        else if (a == "--msg-size" || a == "-m") o.msg_size = std::atoi(next());
        else if (a == "--max-samples") o.max_samples = std::atoi(next());
        else if (a == "-h" || a == "--help") {
            std::printf(
                "Usage: %s [--host H] [--port P] [-t threads] [-c conns]\n"
                "       [-d duration_s] [--warmup s] [-m msg_bytes] [--max-samples N]\n",
                argv[0]);
            std::exit(0);
        }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); std::exit(2); }
    }
    return o;
}

// -----------------------------------------------------------------------------
// 简单的 latency histogram —— 用 vector<uint32_t> 存样本，结束合并排序
// -----------------------------------------------------------------------------
struct LatencyRecorder {
    std::vector<uint32_t> samples_us;   // 微秒
    int max_samples;
    uint64_t total_seen = 0;

    explicit LatencyRecorder(int cap) : max_samples(cap) { samples_us.reserve(cap); }

    inline void record(uint32_t us) {
        ++total_seen;
        if ((int)samples_us.size() < max_samples) {
            samples_us.push_back(us);
        } else {
            // Reservoir sampling：保证均匀分布
            uint64_t k = (uint64_t)(total_seen - 1);
            // 简单 LCG 避免每次 rand() 调用开销
            static thread_local uint64_t s = 0x9E3779B97F4A7C15ULL;
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            uint64_t r = s % total_seen;
            if (r < (uint64_t)max_samples) samples_us[r] = us;
        }
    }
};

// -----------------------------------------------------------------------------
// 单线程状态
// -----------------------------------------------------------------------------
struct ThreadStat {
    uint64_t messages = 0;
    uint64_t bytes_in = 0;
    uint64_t bytes_out = 0;
    uint64_t err_connect = 0;
    uint64_t err_read = 0;
    uint64_t err_write = 0;
    LatencyRecorder lat;
    explicit ThreadStat(int cap) : lat(cap) {}
};

struct Conn {
    int fd = -1;
    std::vector<char> read_buf;
    int read_off = 0;
    std::chrono::steady_clock::time_point send_time;
    bool warming = true;
};

// -----------------------------------------------------------------------------
// connect + set nonblocking + TCP_NODELAY
// -----------------------------------------------------------------------------
int make_conn(const std::string& host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(fd, (sockaddr*)&addr, sizeof addr) < 0) {
        ::close(fd);
        return -1;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

// -----------------------------------------------------------------------------
// 每线程主循环
// -----------------------------------------------------------------------------
void run_worker(int conn_count, const Options& opt,
                std::atomic<bool>& warmup_done,
                std::atomic<bool>& stop_flag,
                ThreadStat& stat) {
    int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { std::perror("epoll_create1"); return; }

    std::string payload(opt.msg_size, 'A');
    payload.back() = '\n';   // 让 server 端纯 echo，方便观察

    std::vector<Conn> conns(conn_count);
    for (int i = 0; i < conn_count; ++i) {
        conns[i].fd = make_conn(opt.host, opt.port);
        if (conns[i].fd < 0) { ++stat.err_connect; continue; }
        conns[i].read_buf.resize(opt.msg_size);
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.u32 = (uint32_t)i;
        ::epoll_ctl(epfd, EPOLL_CTL_ADD, conns[i].fd, &ev);
        // 启动：发第一条消息
        conns[i].send_time = std::chrono::steady_clock::now();
        ssize_t w = ::send(conns[i].fd, payload.data(), payload.size(), MSG_NOSIGNAL);
        if (w < 0) { ++stat.err_write; }
        else { stat.bytes_out += w; }
        // 暂时关掉 EPOLLOUT 避免被反复唤醒
        ev.events = EPOLLIN;
        ::epoll_ctl(epfd, EPOLL_CTL_MOD, conns[i].fd, &ev);
    }

    epoll_event evs[64];
    while (!stop_flag.load(std::memory_order_relaxed)) {
        int n = ::epoll_wait(epfd, evs, 64, 100);  // 100ms 超时让 stop 检测有响应
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait"); break;
        }
        bool in_warmup = !warmup_done.load(std::memory_order_relaxed);

        for (int j = 0; j < n; ++j) {
            uint32_t idx = evs[j].data.u32;
            Conn& c = conns[idx];
            if (c.fd < 0) continue;
            if (evs[j].events & (EPOLLERR | EPOLLHUP)) {
                ++stat.err_read;
                ::close(c.fd);
                c.fd = -1;
                continue;
            }
            if (evs[j].events & EPOLLIN) {
                while (true) {
                    ssize_t r = ::recv(c.fd, c.read_buf.data() + c.read_off,
                                       opt.msg_size - c.read_off, 0);
                    if (r > 0) {
                        c.read_off += (int)r;
                        stat.bytes_in += r;
                        if (c.read_off >= opt.msg_size) {
                            // 完成一个 RTT
                            if (!in_warmup) {
                                auto now = std::chrono::steady_clock::now();
                                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                              now - c.send_time).count();
                                stat.lat.record((uint32_t)us);
                                ++stat.messages;
                            }
                            c.read_off = 0;
                            // 立刻发下一条
                            c.send_time = std::chrono::steady_clock::now();
                            ssize_t w = ::send(c.fd, payload.data(), payload.size(),
                                               MSG_NOSIGNAL);
                            if (w < 0) { ++stat.err_write; }
                            else { stat.bytes_out += w; }
                        }
                    } else if (r == 0) {
                        // 对端关闭
                        ::close(c.fd); c.fd = -1; break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        ++stat.err_read;
                        ::close(c.fd); c.fd = -1; break;
                    }
                }
            }
        }
    }

    for (auto& c : conns) if (c.fd >= 0) ::close(c.fd);
    ::close(epfd);
}

// -----------------------------------------------------------------------------
// 合并 + 计算百分位
// -----------------------------------------------------------------------------
struct LatencyReport {
    uint32_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, min_v = 0, max_v = 0;
    size_t sample_count = 0;
};

LatencyReport compute_percentiles(std::vector<ThreadStat>& stats) {
    std::vector<uint32_t> all;
    size_t total = 0;
    for (auto& s : stats) total += s.lat.samples_us.size();
    all.reserve(total);
    for (auto& s : stats) {
        all.insert(all.end(), s.lat.samples_us.begin(), s.lat.samples_us.end());
    }
    LatencyReport r;
    r.sample_count = all.size();
    if (all.empty()) return r;
    std::sort(all.begin(), all.end());
    auto pick = [&](double q) {
        size_t i = (size_t)(q * (all.size() - 1));
        return all[i];
    };
    r.min_v = all.front();
    r.max_v = all.back();
    r.p50  = pick(0.50);
    r.p90  = pick(0.90);
    r.p99  = pick(0.99);
    r.p999 = pick(0.999);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);

    // 忽略 SIGPIPE：对端关闭时 send 返回 -EPIPE 由我们处理，不要中断进程
    ::signal(SIGPIPE, SIG_IGN);

    std::printf("THREADS=%d CONN=%d MSG=%dB WARMUP=%ds DUR=%ds host=%s:%u\n",
                opt.threads, opt.connections, opt.msg_size,
                opt.warmup_s, opt.duration_s, opt.host.c_str(), opt.port);

    std::vector<ThreadStat> stats;
    stats.reserve(opt.threads);
    for (int i = 0; i < opt.threads; ++i) stats.emplace_back(opt.max_samples);

    std::atomic<bool> warmup_done{false};
    std::atomic<bool> stop_flag{false};

    // 启动线程
    int per_thread = (opt.connections + opt.threads - 1) / opt.threads;
    std::vector<std::thread> threads;
    for (int i = 0; i < opt.threads; ++i) {
        int n = std::min(per_thread, opt.connections - per_thread * i);
        if (n <= 0) break;
        threads.emplace_back(run_worker, n, std::cref(opt),
                             std::ref(warmup_done), std::ref(stop_flag),
                             std::ref(stats[i]));
    }

    // 计时：warmup → 标记 → 正式段
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(opt.warmup_s));
    warmup_done.store(true);
    auto t_meas_start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(opt.duration_s));
    auto t_meas_end = std::chrono::steady_clock::now();
    stop_flag.store(true);

    for (auto& th : threads) th.join();
    (void)t0;

    // 汇总
    uint64_t total_msg = 0, bytes_in = 0, bytes_out = 0;
    uint64_t err_conn = 0, err_r = 0, err_w = 0;
    for (auto& s : stats) {
        total_msg += s.messages;
        bytes_in  += s.bytes_in;
        bytes_out += s.bytes_out;
        err_conn  += s.err_connect;
        err_r     += s.err_read;
        err_w     += s.err_write;
    }
    auto secs = std::chrono::duration<double>(t_meas_end - t_meas_start).count();
    double qps = total_msg / secs;
    double bytes_per_sec = (bytes_in + bytes_out) / secs;
    LatencyReport lr = compute_percentiles(stats);

    std::printf("\n=== Result ===\n");
    std::printf("messages=%lu qps=%.0f bytes/s=%.2f MB/s\n",
                (unsigned long)total_msg, qps, bytes_per_sec / 1e6);
    std::printf("latency_us: p50=%u p90=%u p99=%u p999=%u min=%u max=%u (n=%zu)\n",
                lr.p50, lr.p90, lr.p99, lr.p999, lr.min_v, lr.max_v, lr.sample_count);
    std::printf("errors: connect=%lu read=%lu write=%lu\n",
                (unsigned long)err_conn, (unsigned long)err_r, (unsigned long)err_w);
    return 0;
}
