#pragma once
// 轻量测试断言工具 —— 供 reactor 加固测试统一使用。
// 不引第三方框架,风格沿用 test_bytearray_smoke.cpp 的 EXPECT 宏。
// 用法:
//   #include "test_util.h"
//   TEST_CHECK(cond);            // 布尔断言
//   TEST_CHECK_EQ(a, b);         // 相等断言(打印两侧值)
//   TEST_CHECK_MSG(cond, msg);   // 带说明
//   return TEST_SUMMARY();       // main 末尾:打印统计,有失败返回非零退出码
//
// 多线程安全:计数用 atomic,日志走 BRONX_LOG(线程安全)。

#include "log.h"
#include <atomic>
#include <chrono>

namespace bronx_test {

inline bronx::BxLogger::ptr& logger() {
    static bronx::BxLogger::ptr lg = BRONX_LOG_ROOT();
    return lg;
}

inline std::atomic<int>& checks() { static std::atomic<int> c{0}; return c; }
inline std::atomic<int>& failed() { static std::atomic<int> f{0}; return f; }

// 当前耗时(毫秒),用于超时类断言(如"协程应在 X ms 内被唤醒返回")
inline uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace bronx_test

#define TEST_CHECK_MSG(expr, msg)                                              \
    do {                                                                       \
        ++bronx_test::checks();                                                \
        if(!(expr)) {                                                          \
            ++bronx_test::failed();                                            \
            BRONX_LOG_ERROR(bronx_test::logger())                              \
                << "FAIL: " #expr << " | " << msg                              \
                << " @ " << __FILE__ << ":" << __LINE__;                       \
        }                                                                      \
    } while(0)

#define TEST_CHECK(expr) TEST_CHECK_MSG(expr, "")

#define TEST_CHECK_EQ(a, b)                                                    \
    do {                                                                       \
        ++bronx_test::checks();                                                \
        auto _va = (a); auto _vb = (b);                                        \
        if(!(_va == _vb)) {                                                    \
            ++bronx_test::failed();                                            \
            BRONX_LOG_ERROR(bronx_test::logger())                             \
                << "FAIL: " #a " == " #b " | got " << _va << " vs " << _vb     \
                << " @ " << __FILE__ << ":" << __LINE__;                       \
        }                                                                      \
    } while(0)

// 主动标记一次失败(用于"不该到达此处"的路径)
#define TEST_FAIL(msg)                                                         \
    do {                                                                       \
        ++bronx_test::checks();                                                \
        ++bronx_test::failed();                                                \
        BRONX_LOG_ERROR(bronx_test::logger()) << "FAIL(forced): " << msg       \
            << " @ " << __FILE__ << ":" << __LINE__;                           \
    } while(0)

// main 末尾调用:打印统计并返回退出码(0=全过,1=有失败)
#define TEST_SUMMARY()                                                         \
    ([]() -> int {                                                             \
        int c = bronx_test::checks().load();                                   \
        int f = bronx_test::failed().load();                                   \
        if(f == 0) {                                                           \
            BRONX_LOG_INFO(bronx_test::logger())                               \
                << "ALL PASS: " << c << " checks";                             \
            return 0;                                                          \
        }                                                                      \
        BRONX_LOG_ERROR(bronx_test::logger())                                  \
            << "FAILED: " << f << "/" << c << " checks";                       \
        return 1;                                                              \
    })()
