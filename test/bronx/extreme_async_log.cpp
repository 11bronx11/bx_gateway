// 异步日志极端场景测试
// 更深入的边界探测:内存、时间、配置变更、异常路径

#include "async_logger.h"
#include "log.h"
#include "thread.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <signal.h>
#include <sys/resource.h>

using namespace bronx;
using namespace std::chrono_literals;

static int g_failed = 0;
#define EXPECT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << msg << "\n"; \
            ++g_failed; \
        } else { \
            std::cout << "[ OK ] " << msg << "\n"; \
        } \
    } while (0)

static int countLines(const std::string& path) {
    std::ifstream ifs(path);
    int n = 0;
    std::string line;
    while (std::getline(ifs, line)) ++n;
    return n;
}

static void resetFile(const std::string& path) {
    std::ofstream ofs(path, std::ios::trunc);
}

static void removeFile(const std::string& path) {
    std::remove(path.c_str());
}

static BxLogger::ptr makeAsyncLogger(const std::string& name, const std::string& file) {
    auto logger = LoggerMgr::GetInstance()->getLogger(name);
    logger->clearAppenders();
    logger->setLevel(BxLogLevel::DEBUG);
    auto fa = std::make_shared<BxFileLogAppender>(file);
    fa->setAsync(true);
    fa->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));
    logger->addAppender(fa);
    return logger;
}

// 极端1: 超大消息(测试内存分配与 format 性能)
static void test_huge_messages() {
    std::cout << "\n=== Extreme 1: Huge messages (1MB each) ===\n";
    const std::string f = "/tmp/extreme_huge.log";
    resetFile(f);
    auto lg = makeAsyncLogger("extreme1", f);

    constexpr int N = 100;
    constexpr size_t MSG_SIZE = 1024 * 1024;  // 1MB
    std::string huge(MSG_SIZE, 'X');

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        BRONX_LOG_INFO(lg) << "huge-" << i << "-" << huge;
    }
    BxAsyncLoggerMgr::GetInstance()->flushAll();
    auto t1 = std::chrono::steady_clock::now();

    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    int lines = countLines(f);
    EXPECT(lines == N, "Huge messages: " << lines << " lines (expected " << N << "), took " << elapsed << "s");
    lg->clearAppenders();
    removeFile(f);
}

// 极端2: 持续高频(10秒不间断狂打,测试稳定性)
static void test_sustained_high_rate() {
    std::cout << "\n=== Extreme 2: Sustained high rate (10 seconds) ===\n";
    const std::string f = "/tmp/extreme_sustained.log";
    resetFile(f);
    auto lg = makeAsyncLogger("extreme2", f);

    constexpr int N_THREADS = 8;
    std::atomic<bool> stop{false};
    std::atomic<int64_t> count{0};

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (int i = 0; i < N_THREADS; ++i) {
        workers.emplace_back([&, i]{
            int local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                BRONX_LOG_INFO(lg) << "sustained-T" << i << "-" << local++;
                count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(10));
    stop.store(true);

    for (auto& w : workers) w.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();
    auto t1 = std::chrono::steady_clock::now();

    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    int64_t produced = count.load();
    int lines = countLines(f);

    std::cout << "  Produced: " << produced << " messages in " << elapsed << "s\n";
    std::cout << "  Rate: " << static_cast<int64_t>(produced / elapsed) << " msg/s\n";
    EXPECT(lines == produced, "Sustained: " << lines << " lines match produced " << produced);
    lg->clearAppenders();
    removeFile(f);
}

// 极端3: FATAL 混合(FATAL 应该立即 flushAll,然后同步写)
static void test_fatal_interleaved() {
    std::cout << "\n=== Extreme 3: FATAL interleaved with INFO ===\n";
    const std::string f = "/tmp/extreme_fatal.log";
    resetFile(f);
    auto lg = makeAsyncLogger("extreme3", f);

    // 先写一批 INFO
    for (int i = 0; i < 1000; ++i) {
        BRONX_LOG_INFO(lg) << "info-before-fatal-" << i;
    }

    // 写 FATAL(应该触发 flushAll,前面 1000 条全部落盘)
    BRONX_LOG_FATAL(lg) << "FATAL_MARKER";

    // 再写一批 INFO
    for (int i = 0; i < 1000; ++i) {
        BRONX_LOG_INFO(lg) << "info-after-fatal-" << i;
    }

    BxAsyncLoggerMgr::GetInstance()->flushAll();

    // 检查 FATAL_MARKER 前有 1000 行,总共 2001 行
    std::ifstream ifs(f);
    std::string line;
    int idx = 0;
    int fatal_idx = -1;
    while (std::getline(ifs, line)) {
        if (line.find("FATAL_MARKER") != std::string::npos) {
            fatal_idx = idx;
        }
        ++idx;
    }

    EXPECT(idx == 2001, "FATAL interleaved: " << idx << " total lines (expected 2001)");
    EXPECT(fatal_idx == 1000, "FATAL at line " << fatal_idx << " (expected 1000)");
    lg->clearAppenders();
    removeFile(f);
}

// 极端4: 配置热更新(clearAppenders 后旧队列中的日志仍能安全写完)
static void test_hot_config_change() {
    std::cout << "\n=== Extreme 4: Hot config change (clearAppenders mid-flight) ===\n";
    const std::string f1 = "/tmp/extreme_config1.log";
    const std::string f2 = "/tmp/extreme_config2.log";
    resetFile(f1);
    resetFile(f2);

    auto lg = makeAsyncLogger("extreme4", f1);

    std::atomic<bool> stop{false};
    std::atomic<int> count{0};

    // producer: 持续写
    std::thread producer([&]{
        int local = 0;
        while (!stop.load()) {
            BRONX_LOG_INFO(lg) << "config-msg-" << local++;
            count.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 中途切换 appender(旧的在队列里的日志应该仍写到 f1)
    lg->clearAppenders();
    auto fa2 = std::make_shared<BxFileLogAppender>(f2);
    fa2->setAsync(true);
    fa2->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));
    lg->addAppender(fa2);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
    producer.join();

    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int lines1 = countLines(f1);
    int lines2 = countLines(f2);
    int total = lines1 + lines2;
    int expected = count.load();

    std::cout << "  File1: " << lines1 << " lines\n";
    std::cout << "  File2: " << lines2 << " lines\n";
    std::cout << "  Total: " << total << " (expected " << expected << ")\n";
    EXPECT(total == expected, "Hot config: total matches produced");
    lg->clearAppenders();
    removeFile(f1);
    removeFile(f2);
}

// 极端5: 线程在 push 中被信号打断(测试 eintr 容错)
static std::atomic<bool> g_signal_fired{false};
static void signal_handler(int) {
    g_signal_fired.store(true);
}

static void test_signal_interruption() {
    std::cout << "\n=== Extreme 5: Signal interruption during push ===\n";
    const std::string f = "/tmp/extreme_signal.log";
    resetFile(f);
    auto lg = makeAsyncLogger("extreme5", f);

    signal(SIGUSR1, signal_handler);

    std::atomic<bool> ready{false};
    std::atomic<int> count{0};

    std::thread worker([&]{
        ready.store(true);
        for (int i = 0; i < 10000; ++i) {
            BRONX_LOG_INFO(lg) << "signal-msg-" << i;
            count.fetch_add(1);
        }
    });

    while (!ready.load()) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // 发信号打断
    pthread_kill(worker.native_handle(), SIGUSR1);

    worker.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int lines = countLines(f);
    int expected = count.load();
    EXPECT(lines == expected && g_signal_fired.load(),
           "Signal: " << lines << " lines (expected " << expected << "), signal fired = " << g_signal_fired.load());

    signal(SIGUSR1, SIG_DFL);
    lg->clearAppenders();
    removeFile(f);
}

// 极端6: 内存限制(ulimit 式,模拟 OOM 压力)
static void test_memory_pressure() {
    std::cout << "\n=== Extreme 6: Memory pressure (many buffers in flight) ===\n";
    const std::string f = "/tmp/extreme_memory.log";
    resetFile(f);
    auto lg = makeAsyncLogger("extreme6", f);

    // 缩小 buffer 让它频繁分配/回收
    size_t oldBuf = BxAsyncLogger::kBufferCapacity;
    BxAsyncLogger::kBufferCapacity = 64;

    constexpr int N_THREADS = 32;
    constexpr int N_PER = 5000;
    std::atomic<bool> start{false};

    std::vector<std::thread> workers;
    for (int i = 0; i < N_THREADS; ++i) {
        workers.emplace_back([&, i]{
            while (!start.load()) {}
            for (int j = 0; j < N_PER; ++j) {
                BRONX_LOG_INFO(lg) << "mem-T" << i << "-" << j;
            }
        });
    }

    start.store(true);
    for (auto& w : workers) w.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    BxAsyncLogger::kBufferCapacity = oldBuf;

    int lines = countLines(f);
    int expected = N_THREADS * N_PER;
    EXPECT(lines == expected, "Memory pressure: " << lines << " lines (expected " << expected << ")");
    lg->clearAppenders();
    removeFile(f);
}

// 极端7: 线程本地存储爆炸(大量线程同时创建 TLS,测试 m_tlsList 性能)
static void test_tls_explosion() {
    std::cout << "\n=== Extreme 7: TLS explosion (500 threads) ===\n";
    const std::string f = "/tmp/extreme_tls_boom.log";
    resetFile(f);
    auto lg = makeAsyncLogger("extreme7", f);

    constexpr int N_THREADS = 500;
    constexpr int N_PER = 100;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (int i = 0; i < N_THREADS; ++i) {
        workers.emplace_back([&, i]{
            ready.fetch_add(1);
            while (!start.load()) {}
            for (int j = 0; j < N_PER; ++j) {
                BRONX_LOG_INFO(lg) << "tls-T" << i << "-" << j;
            }
        });
    }

    while (ready.load() < N_THREADS) {}
    start.store(true);

    for (auto& w : workers) w.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();
    auto t1 = std::chrono::steady_clock::now();

    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    int lines = countLines(f);
    int expected = N_THREADS * N_PER;

    std::cout << "  Time: " << elapsed << "s\n";
    EXPECT(lines == expected, "TLS explosion: " << lines << " lines (expected " << expected << ")");
    lg->clearAppenders();
    removeFile(f);
}

// 极端8: 循环 shutdown + 重启(leaky singleton 后 shutdown 不真正销毁对象,但要幂等)
static void test_repeated_shutdown_restart() {
    std::cout << "\n=== Extreme 8: Repeated shutdown (idempotent) ===\n";
    const std::string f = "/tmp/extreme_repeat_shutdown.log";
    resetFile(f);
    auto lg = makeAsyncLogger("extreme8", f);

    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 100; ++i) {
            BRONX_LOG_INFO(lg) << "round-" << round << "-msg-" << i;
        }
        BxAsyncLoggerMgr::GetInstance()->shutdown();
        // shutdown 后再写(应该走同步路径)
        for (int i = 0; i < 10; ++i) {
            BRONX_LOG_INFO(lg) << "round-" << round << "-post-shutdown-" << i;
        }
    }

    int lines = countLines(f);
    int expected = 5 * (100 + 10);
    EXPECT(lines == expected, "Repeated shutdown: " << lines << " lines (expected " << expected << ")");
    lg->clearAppenders();
    removeFile(f);
}

// 极端9: 零长度消息
static void test_empty_messages() {
    std::cout << "\n=== Extreme 9: Empty messages ===\n";
    const std::string f = "/tmp/extreme_empty.log";
    resetFile(f);
    auto lg = makeAsyncLogger("extreme9", f);

    for (int i = 0; i < 1000; ++i) {
        BRONX_LOG_INFO(lg) << "";  // 空消息
    }
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int lines = countLines(f);
    EXPECT(lines == 1000, "Empty messages: " << lines << " lines (expected 1000)");
    lg->clearAppenders();
    removeFile(f);
}

// 极端10: 多 logger 交叉写(测试全局单例的隔离性)
static void test_multi_logger_cross() {
    std::cout << "\n=== Extreme 10: Multiple loggers cross-write ===\n";
    const std::string f1 = "/tmp/extreme_lg1.log";
    const std::string f2 = "/tmp/extreme_lg2.log";
    const std::string f3 = "/tmp/extreme_lg3.log";
    resetFile(f1);
    resetFile(f2);
    resetFile(f3);

    auto lg1 = makeAsyncLogger("multi1", f1);
    auto lg2 = makeAsyncLogger("multi2", f2);
    auto lg3 = makeAsyncLogger("multi3", f3);

    constexpr int N_THREADS = 9;  // 3 threads per logger
    constexpr int N_PER = 1000;
    std::atomic<bool> start{false};

    std::vector<std::thread> workers;
    for (int i = 0; i < N_THREADS; ++i) {
        auto lg = (i % 3 == 0) ? lg1 : (i % 3 == 1) ? lg2 : lg3;
        std::string prefix = "lg" + std::to_string(i % 3 + 1);
        workers.emplace_back([=, &start]{
            while (!start.load()) {}
            for (int j = 0; j < N_PER; ++j) {
                BRONX_LOG_INFO(lg) << prefix << "-T" << i << "-" << j;
            }
        });
    }

    start.store(true);
    for (auto& w : workers) w.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int lines1 = countLines(f1);
    int lines2 = countLines(f2);
    int lines3 = countLines(f3);

    EXPECT(lines1 == 3 * N_PER, "Multi-logger lg1: " << lines1 << " (expected " << 3 * N_PER << ")");
    EXPECT(lines2 == 3 * N_PER, "Multi-logger lg2: " << lines2 << " (expected " << 3 * N_PER << ")");
    EXPECT(lines3 == 3 * N_PER, "Multi-logger lg3: " << lines3 << " (expected " << 3 * N_PER << ")");
    lg1->clearAppenders();
    lg2->clearAppenders();
    lg3->clearAppenders();
    removeFile(f1);
    removeFile(f2);
    removeFile(f3);
}

int main() {
    std::cout << "Async BxLogger Extreme Scenario Testing\n";
    std::cout << "======================================\n";

    test_huge_messages();
    test_sustained_high_rate();
    test_fatal_interleaved();
    test_hot_config_change();
    test_signal_interruption();
    test_memory_pressure();
    test_tls_explosion();
    test_repeated_shutdown_restart();
    test_empty_messages();
    test_multi_logger_cross();

    std::cout << "\n=== Summary ===\n";
    if (g_failed == 0) {
        std::cout << "All extreme tests passed ✓\n";
        return 0;
    } else {
        std::cout << g_failed << " test(s) failed ✗\n";
        return 1;
    }
}
