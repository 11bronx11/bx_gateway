// 异步日志边界/压力测试
// 目标:找出并发、资源限制、生命周期的边界 bug

#include "async_logger.h"
#include "log.h"
#include "thread.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include <unistd.h>

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

static BxLogger::ptr makeAsyncLogger(const std::string& name, const std::string& file) {
    auto logger = LoggerMgr::GetInstance()->getLogger(name);
    logger->clearAppenders();
    logger->setLevel(BxLogLevel::INFO);
    auto fa = std::make_shared<BxFileLogAppender>(file);
    fa->setAsync(true);
    fa->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));
    logger->addAppender(fa);
    return logger;
}

// 边界1: 大量线程同时创建 TLS(测试 getOrCreateTls 的并发)
static void test_massive_thread_creation() {
    std::cout << "\n=== Stress 1: Massive thread TLS creation ===\n";
    const std::string f = "/tmp/stress_tls.log";
    resetFile(f);
    auto lg = makeAsyncLogger("stress1", f);

    constexpr int N_THREADS = 100;
    constexpr int N_PER = 1000;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> workers;
    for (int i = 0; i < N_THREADS; ++i) {
        workers.emplace_back([&, i]{
            ready.fetch_add(1);
            while (!start.load()) {}
            for (int j = 0; j < N_PER; ++j) {
                BRONX_LOG_INFO(lg) << "T" << i << "-" << j;
            }
        });
    }

    while (ready.load() < N_THREADS) {}
    start.store(true);
    for (auto& w : workers) w.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int lines = countLines(f);
    EXPECT(lines == N_THREADS * N_PER,
           "100 threads × 1000 msgs = " << lines << " (expected " << N_THREADS * N_PER << ")");
}

// 边界2: 线程快速创建/销毁(测试 TLS 析构器竞态)
static void test_rapid_thread_lifecycle() {
    std::cout << "\n=== Stress 2: Rapid thread create/destroy ===\n";
    const std::string f = "/tmp/stress_lifecycle.log";
    resetFile(f);
    auto lg = makeAsyncLogger("stress2", f);

    std::atomic<int> total{0};
    constexpr int N_ROUNDS = 50;
    constexpr int N_THREADS_PER_ROUND = 20;
    constexpr int N_PER = 100;

    for (int round = 0; round < N_ROUNDS; ++round) {
        std::vector<std::thread> workers;
        for (int i = 0; i < N_THREADS_PER_ROUND; ++i) {
            workers.emplace_back([&, round, i]{
                for (int j = 0; j < N_PER; ++j) {
                    BRONX_LOG_INFO(lg) << "R" << round << "T" << i << "M" << j;
                }
                total.fetch_add(N_PER);
            });
        }
        for (auto& w : workers) w.join();
        // 快速下一轮,TLS 析构器与新线程的 getOrCreateTls 并发
    }

    BxAsyncLoggerMgr::GetInstance()->flushAll();
    int lines = countLines(f);
    int expected = N_ROUNDS * N_THREADS_PER_ROUND * N_PER;
    EXPECT(lines == expected,
           "Rapid lifecycle: " << lines << " lines (expected " << expected << ")");
}

// 边界3: 心跳与 flushAll 并发(测试 m_tlsMutex 争用)
static void test_heartbeat_flush_race() {
    std::cout << "\n=== Stress 3: Heartbeat vs flushAll race ===\n";
    const std::string f = "/tmp/stress_hb_flush.log";
    resetFile(f);
    auto lg = makeAsyncLogger("stress3", f);

    // 缩短心跳周期让它频繁触发
    unsigned oldHb = BxAsyncLogger::kHeartbeatMs;
    BxAsyncLogger::kHeartbeatMs = 10;  // 10ms 一次心跳

    std::atomic<bool> stop{false};
    std::atomic<int> produced{0};

    // producer: 慢速写,让 TLS buffer 不满,依赖心跳刷
    std::thread producer([&]{
        for (int i = 0; i < 5000 && !stop.load(); ++i) {
            BRONX_LOG_INFO(lg) << "hb-msg-" << i;
            produced.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // flusher: 并发调 flushAll(与心跳竞争 m_tlsMutex)
    std::thread flusher([&]{
        for (int i = 0; i < 100 && !stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            BxAsyncLoggerMgr::GetInstance()->flushAll();
        }
    });

    producer.join();
    stop.store(true);
    flusher.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    BxAsyncLogger::kHeartbeatMs = oldHb;  // 恢复

    int lines = countLines(f);
    int expected = produced.load();
    EXPECT(lines == expected,
           "Heartbeat race: " << lines << " lines (expected " << expected << ")");
}

// 边界4: 队列持续满载(测试背压 + writer 能否跟上)
static void test_sustained_full_queue() {
    std::cout << "\n=== Stress 4: Sustained queue saturation ===\n";
    const std::string f = "/tmp/stress_full_queue.log";
    resetFile(f);
    auto lg = makeAsyncLogger("stress4", f);

    size_t oldCap = BxAsyncLogger::kCentralCapacity;
    BxAsyncLogger::kCentralCapacity = 8;  // 极小队列

    constexpr int N_THREADS = 16;
    constexpr int N_PER = 5000;
    std::atomic<bool> start{false};

    std::vector<std::thread> workers;
    for (int i = 0; i < N_THREADS; ++i) {
        workers.emplace_back([&, i]{
            while (!start.load()) {}
            for (int j = 0; j < N_PER; ++j) {
                BRONX_LOG_INFO(lg) << "saturation-T" << i << "-" << j;
            }
        });
    }

    start.store(true);
    for (auto& w : workers) w.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    BxAsyncLogger::kCentralCapacity = oldCap;

    int lines = countLines(f);
    int expected = N_THREADS * N_PER;
    EXPECT(lines == expected,
           "Queue saturation: " << lines << " lines (expected " << expected << ")");
}

// 边界5: 多次 shutdown(测试幂等性)
static void test_multiple_shutdown() {
    std::cout << "\n=== Stress 5: Multiple shutdown calls ===\n";
    const std::string f = "/tmp/stress_shutdown.log";
    resetFile(f);
    auto lg = makeAsyncLogger("stress5", f);

    for (int i = 0; i < 1000; ++i) {
        BRONX_LOG_INFO(lg) << "pre-shutdown-" << i;
    }

    // 多个线程同时调 shutdown
    std::vector<std::thread> shutdowners;
    for (int i = 0; i < 10; ++i) {
        shutdowners.emplace_back([]{
            BxAsyncLoggerMgr::GetInstance()->shutdown();
        });
    }
    for (auto& s : shutdowners) s.join();

    // 再次尝试写(应该走同步路径)
    for (int i = 0; i < 100; ++i) {
        BRONX_LOG_INFO(lg) << "post-shutdown-" << i;
    }

    int lines = countLines(f);
    EXPECT(lines == 1100,
           "Multiple shutdown: " << lines << " lines (expected 1100, shutdown is idempotent)");
}

// 边界6: buffer 边界(恰好 1024, 1023, 1025 条)
static void test_buffer_boundary() {
    std::cout << "\n=== Stress 6: Buffer capacity boundary ===\n";
    const std::string f = "/tmp/stress_boundary.log";
    resetFile(f);
    auto lg = makeAsyncLogger("stress6", f);

    size_t cap = BxAsyncLogger::kBufferCapacity;  // 通常 1024

    // 写 cap-1 条(不触发 swap)
    for (size_t i = 0; i < cap - 1; ++i) {
        BRONX_LOG_INFO(lg) << "boundary-" << i;
    }
    // 写第 cap 条(触发 swap)
    BRONX_LOG_INFO(lg) << "boundary-trigger";
    // 写 cap+1 条(新 buffer)
    BRONX_LOG_INFO(lg) << "boundary-after";

    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int lines = countLines(f);
    EXPECT(lines == cap + 1,
           "Buffer boundary: " << lines << " lines (expected " << cap + 1 << ")");
}

// 边界7: 零日志(空 TLS,心跳不应 crash)
static void test_empty_tls() {
    std::cout << "\n=== Stress 7: Empty TLS (heartbeat with no data) ===\n";
    const std::string f = "/tmp/stress_empty.log";
    resetFile(f);
    auto lg = makeAsyncLogger("stress7", f);

    // 创建 TLS 但不写日志
    std::thread t([&]{
        // getOrCreateTls 隐式创建
        BRONX_LOG_INFO(lg) << "create-tls";
        // 然后什么也不做,让心跳来 swap 空 buffer
    });
    t.join();

    // 等几个心跳周期
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int lines = countLines(f);
    EXPECT(lines == 1, "Empty TLS: " << lines << " line (heartbeat should not crash on empty)");
}

// 边界8: 随机长度消息(测试 format 性能与内存分配)
static void test_random_message_length() {
    std::cout << "\n=== Stress 8: Random message lengths ===\n";
    const std::string f = "/tmp/stress_random_len.log";
    resetFile(f);
    auto lg = makeAsyncLogger("stress8", f);

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(10, 1000);

    constexpr int N = 10000;
    for (int i = 0; i < N; ++i) {
        int len = dist(rng);
        std::string payload(len, 'x');
        BRONX_LOG_INFO(lg) << "random-" << i << "-" << payload;
    }

    BxAsyncLoggerMgr::GetInstance()->flushAll();
    int lines = countLines(f);
    EXPECT(lines == N, "Random lengths: " << lines << " lines (expected " << N << ")");
}

// 边界9: 极端情况 - shutdown 时仍有线程在 push
static void test_shutdown_with_active_pushers() {
    std::cout << "\n=== Stress 9: Shutdown while threads actively pushing ===\n";
    const std::string f = "/tmp/stress_active_push.log";
    resetFile(f);
    auto lg = makeAsyncLogger("stress9", f);

    std::atomic<bool> stop{false};
    std::atomic<int> count{0};

    constexpr int N_THREADS = 8;
    std::vector<std::thread> workers;
    for (int i = 0; i < N_THREADS; ++i) {
        workers.emplace_back([&, i]{
            int local = 0;
            while (!stop.load()) {
                BRONX_LOG_INFO(lg) << "active-T" << i << "-" << local++;
                count.fetch_add(1);
            }
        });
    }

    // 让它们跑一会儿
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 突然 shutdown(此时线程还在狂写)
    BxAsyncLoggerMgr::GetInstance()->shutdown();
    stop.store(true);

    for (auto& w : workers) w.join();

    int lines = countLines(f);
    int produced = count.load();
    // shutdown 后的同步写也会落盘,所以 lines >= produced 的一部分
    EXPECT(lines > 0 && lines <= produced,
           "Active push shutdown: " << lines << " lines written (produced " << produced << ", some went sync after shutdown)");
}

// 边界10: BxMpscRing 容量非 2 的幂(应该在构造时 throw)
static void test_invalid_queue_capacity() {
    std::cout << "\n=== Stress 10: Invalid queue capacity (non-power-of-2) ===\n";

    size_t oldCap = BxAsyncLogger::kCentralCapacity;
    BxAsyncLogger::kCentralCapacity = 1000;  // 不是 2 的幂

    bool threw = false;
    try {
        // 尝试创建新的 BxAsyncLogger(但我们用 leaky singleton,已经构造过了)
        // 这个测试需要单独跑,或者直接构造 BxMpscRing
        BxMpscRing<int> ring(1000);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::cout << "  Caught expected exception: " << e.what() << "\n";
    }

    BxAsyncLogger::kCentralCapacity = oldCap;
    EXPECT(threw, "Non-power-of-2 capacity throws std::invalid_argument");
}

int main() {
    std::cout << "Async BxLogger Stress/Boundary Testing\n";
    std::cout << "=====================================\n";

    test_massive_thread_creation();
    test_rapid_thread_lifecycle();
    test_heartbeat_flush_race();
    test_sustained_full_queue();
    test_multiple_shutdown();
    test_buffer_boundary();
    test_empty_tls();
    test_random_message_length();
    test_shutdown_with_active_pushers();
    test_invalid_queue_capacity();

    std::cout << "\n=== Summary ===\n";
    if (g_failed == 0) {
        std::cout << "All stress tests passed ✓\n";
        return 0;
    } else {
        std::cout << g_failed << " test(s) failed ✗\n";
        return 1;
    }
}
