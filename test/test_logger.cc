// =============================================================================
// test_logger.cc —— Logger / AsyncLogger 验证
// =============================================================================
#include "coro_net/log.hpp"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <glob.h>

namespace {

std::string find_log(const std::string& prefix) {
    glob_t g;
    std::string pat = prefix + ".*.log";
    if (::glob(pat.c_str(), 0, nullptr, &g) != 0 || g.gl_pathc == 0) {
        ::globfree(&g);
        return {};
    }
    std::string newest = g.gl_pathv[g.gl_pathc - 1];
    ::globfree(&g);
    return newest;
}

void cleanup(const std::string& prefix) {
    glob_t g;
    std::string pat = prefix + ".*.log";
    if (::glob(pat.c_str(), 0, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i) {
            ::unlink(g.gl_pathv[i]);
        }
    }
    ::globfree(&g);
}

int count_lines(const std::string& path) {
    std::ifstream f(path);
    int n = 0;
    std::string line;
    while (std::getline(f, line)) ++n;
    return n;
}

bool file_contains(const std::string& path, const std::string& pat) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(pat) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// -----------------------------------------------------------------------------
// 1. 基本 emit + 行数
// -----------------------------------------------------------------------------
CORO_TEST(logger_basic_emission) {
    const std::string prefix = "/tmp/coro_net_test_logger_basic";
    cleanup(prefix);
    coro_net::AsyncLogger::init(prefix);
    coro_net::Logger::set_global_level(coro_net::LogLevel::INFO);

    for (int i = 0; i < 1000; ++i) {
        LOG_INFO << "msg=" << i;
    }
    coro_net::AsyncLogger::shutdown();

    auto path = find_log(prefix);
    CORO_EXPECT_TRUE(!path.empty());
    CORO_EXPECT_EQ(count_lines(path), 1000);
    CORO_EXPECT_TRUE(file_contains(path, "msg=42"));
    cleanup(prefix);
}

// -----------------------------------------------------------------------------
// 2. 双缓冲翻转：~5MB 数据全部落盘
// -----------------------------------------------------------------------------
CORO_TEST(logger_double_buffer_swap) {
    const std::string prefix = "/tmp/coro_net_test_logger_buffer";
    cleanup(prefix);
    coro_net::AsyncLogger::init(prefix);
    coro_net::Logger::set_global_level(coro_net::LogLevel::INFO);

    std::string body(100, 'x');   // 单行 ~150B
    const int N = 40000;          // ≈ 6MB
    for (int i = 0; i < N; ++i) {
        LOG_INFO << body;
    }
    coro_net::AsyncLogger::shutdown();

    auto path = find_log(prefix);
    CORO_EXPECT_TRUE(!path.empty());
    CORO_EXPECT_EQ(count_lines(path), N);
    cleanup(prefix);
}

// -----------------------------------------------------------------------------
// 3. 运行期级别过滤
// -----------------------------------------------------------------------------
CORO_TEST(logger_runtime_level_filter) {
    const std::string prefix = "/tmp/coro_net_test_logger_filter";
    cleanup(prefix);
    coro_net::AsyncLogger::init(prefix);
    coro_net::Logger::set_global_level(coro_net::LogLevel::WARN);

    for (int i = 0; i < 100; ++i) {
        LOG_INFO << "info-" << i;   // 应被过滤
        LOG_WARN << "warn-" << i;
    }
    coro_net::AsyncLogger::shutdown();

    auto path = find_log(prefix);
    CORO_EXPECT_TRUE(!path.empty());
    CORO_EXPECT_TRUE(!file_contains(path, "info-50"));
    CORO_EXPECT_TRUE(file_contains(path, "warn-50"));
    cleanup(prefix);
}

// -----------------------------------------------------------------------------
// 4. 整数格式化覆盖边界
// -----------------------------------------------------------------------------
CORO_TEST(logger_int_formatting) {
    const std::string prefix = "/tmp/coro_net_test_logger_int";
    cleanup(prefix);
    coro_net::AsyncLogger::init(prefix);
    coro_net::Logger::set_global_level(coro_net::LogLevel::INFO);

    LOG_INFO << "v=" << 0;
    LOG_INFO << "v=" << 1;
    LOG_INFO << "v=" << -1;
    LOG_INFO << "v=" << INT_MIN;
    LOG_INFO << "v=" << INT_MAX;
    LOG_INFO << "v=" << static_cast<unsigned int>(UINT_MAX);

    coro_net::AsyncLogger::shutdown();

    auto path = find_log(prefix);
    CORO_EXPECT_TRUE(!path.empty());
    CORO_EXPECT_TRUE(file_contains(path, "v=0 "));
    CORO_EXPECT_TRUE(file_contains(path, "v=-1 "));
    CORO_EXPECT_TRUE(file_contains(path, "v=-2147483648 "));
    CORO_EXPECT_TRUE(file_contains(path, "v=2147483647 "));
    CORO_EXPECT_TRUE(file_contains(path, "v=4294967295 "));
    cleanup(prefix);
}

// -----------------------------------------------------------------------------
// 5. 并发 append：多个线程同时写，行总数正确，无半截行
// -----------------------------------------------------------------------------
CORO_TEST(logger_concurrent_append) {
    const std::string prefix = "/tmp/coro_net_test_logger_concur";
    cleanup(prefix);
    coro_net::AsyncLogger::init(prefix);
    coro_net::Logger::set_global_level(coro_net::LogLevel::INFO);

    constexpr int N_THREADS = 8;
    constexpr int N_PER     = 2000;
    std::vector<std::thread> ths;
    for (int t = 0; t < N_THREADS; ++t) {
        ths.emplace_back([t] {
            for (int i = 0; i < N_PER; ++i) {
                LOG_INFO << "tid=" << t << " i=" << i;
            }
        });
    }
    for (auto& th : ths) th.join();

    coro_net::AsyncLogger::shutdown();

    auto path = find_log(prefix);
    CORO_EXPECT_TRUE(!path.empty());
    CORO_EXPECT_EQ(count_lines(path), N_THREADS * N_PER);
    cleanup(prefix);
}

// -----------------------------------------------------------------------------
// 6. LOG_FATAL 触发 abort（fork 子进程验证）
// -----------------------------------------------------------------------------
CORO_TEST(logger_fatal_aborts) {
    pid_t pid = fork();
    if (pid == 0) {
        // child
        coro_net::AsyncLogger::init("/tmp/coro_net_test_logger_fatal");
        LOG_FATAL << "die";
        // 不应该到达这里
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CORO_EXPECT_TRUE(WIFSIGNALED(status));
    CORO_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    // 清理子进程产生的日志
    glob_t g;
    if (::glob("/tmp/coro_net_test_logger_fatal.*.log", 0, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i) ::unlink(g.gl_pathv[i]);
    }
    ::globfree(&g);
}

// -----------------------------------------------------------------------------
// 7. 文件按大小滚动
// -----------------------------------------------------------------------------
CORO_TEST(logger_rolling) {
    const std::string prefix = "/tmp/coro_net_test_logger_roll";
    cleanup(prefix);
    coro_net::AsyncLogger::init(prefix, /*roll_size=*/4096);
    coro_net::Logger::set_global_level(coro_net::LogLevel::INFO);

    std::string body(200, 'r');
    for (int i = 0; i < 100; ++i) {
        LOG_INFO << body;
        // 间隔一会儿让后端线程有机会滚动
        if (i % 10 == 9) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    coro_net::AsyncLogger::shutdown();

    glob_t g;
    std::string pat = prefix + ".*.log";
    ::glob(pat.c_str(), 0, nullptr, &g);
    size_t n = g.gl_pathc;
    ::globfree(&g);
    // 至少 2 个文件（roll 至少发生一次）
    CORO_EXPECT_TRUE(n >= 2);
    cleanup(prefix);
}

int main() {
    return coro_test::run_all();
}
