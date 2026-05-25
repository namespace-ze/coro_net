// =============================================================================
// test_util.hpp — 极简测试辅助宏
// =============================================================================
// 教学项目使用的轻量级断言框架，避免引入 gtest 等外部依赖。
// 后续若需要更细粒度的报告（如 fixture / parameterized test），可平滑迁移到 gtest。
//
// 用法:
//   CORO_TEST(test_name) { ... }    // 定义一个测试
//   CORO_EXPECT_EQ(a, b);            // 期望相等
//   CORO_EXPECT_TRUE(expr);          // 期望真
//   int main() { return coro_test::run_all(); }
// =============================================================================
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>

namespace coro_test {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int& failed_count() { static int n = 0; return n; }

inline int run_all() {
    int total = (int)registry().size();
    int passed = 0;
    for (auto& tc : registry()) {
        int before = failed_count();
        std::fprintf(stderr, "[ RUN      ] %s\n", tc.name);
        try {
            tc.fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  UNCAUGHT EXCEPTION: %s\n", e.what());
            failed_count()++;
        } catch (...) {
            std::fprintf(stderr, "  UNCAUGHT UNKNOWN EXCEPTION\n");
            failed_count()++;
        }
        if (failed_count() == before) {
            std::fprintf(stderr, "[       OK ] %s\n", tc.name);
            passed++;
        } else {
            std::fprintf(stderr, "[  FAILED  ] %s\n", tc.name);
        }
    }
    std::fprintf(stderr, "\n[==========] %d tests, %d passed, %d failed\n",
                 total, passed, total - passed);
    return failed_count() == 0 ? 0 : 1;
}

}  // namespace coro_test

#define CORO_TEST(name)                                                  \
    static void name();                                                  \
    static ::coro_test::Registrar reg_##name{#name, name};               \
    static void name()

#define CORO_EXPECT_TRUE(expr)                                           \
    do {                                                                 \
        if (!(expr)) {                                                   \
            std::fprintf(stderr, "  EXPECT_TRUE failed: %s @ %s:%d\n",   \
                         #expr, __FILE__, __LINE__);                     \
            ::coro_test::failed_count()++;                               \
        }                                                                \
    } while (0)

#define CORO_EXPECT_EQ(a, b)                                             \
    do {                                                                 \
        auto _x = (a); auto _y = (b);                                    \
        if (!(_x == _y)) {                                               \
            std::fprintf(stderr,                                         \
                "  EXPECT_EQ failed: %s != %s @ %s:%d\n",                \
                #a, #b, __FILE__, __LINE__);                             \
            ::coro_test::failed_count()++;                               \
        }                                                                \
    } while (0)
