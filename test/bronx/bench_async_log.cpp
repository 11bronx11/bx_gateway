// 异步日志性能压测
// 测试指标:
//   1. 单线程吞吐量 (条/秒)
//   2. 多线程吞吐量 (1/2/4/8 线程)
//   3. 延迟分布 (p50/p95/p99)
//   4. 队列满时的背压行为

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
#include <algorithm>
#include <cmath>

using namespace bronx;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using Us = std::chrono::microseconds;
using Ns = std::chrono::nanoseconds;

static int g_failed = 0;

static void resetFile(const std::string& path) {
    std::ofstream(path, std::ios::trunc);
}

static void finishFile(const BxLogger::ptr& logger, const std::string& path) {
    logger->clearAppenders();
    std::remove(path.c_str());
}

static BxLogger::ptr makeAsyncLogger(const std::string& name, const std::string& file) {
    auto logger = LoggerMgr::GetInstance()->getLogger(name);
    logger->clearAppenders();
    logger->setLevel(BxLogLevel::INFO);
    auto fa = std::make_shared<BxFileLogAppender>(file);
    fa->setAsync(true);
    fa->setFormatter(std::make_shared<BxLogFormatter>("%d{%Y-%m-%d %H:%M:%S} [%p] %m%n"));
    logger->addAppender(fa);
    return logger;
}

// 单线程吞吐量
static void bench_single_thread() {
    std::cout << "\n=== Benchmark 1: Single-thread throughput ===\n";
    const std::string f = "/tmp/bronx_bench_single.log";
    resetFile(f);
    auto lg = makeAsyncLogger("bench1", f);

    constexpr int N = 500000;
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
        BRONX_LOG_INFO(lg) << "Benchmark message #" << i << " with some payload data";
    }
    BxAsyncLoggerMgr::GetInstance()->flushAll();
    auto t1 = Clock::now();

    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    double throughput = N / elapsed;
    std::cout << "  Messages: " << N << "\n";
    std::cout << "  Time: " << elapsed << " s\n";
    std::cout << "  Throughput: " << static_cast<int>(throughput) << " msg/s\n";
    finishFile(lg, f);
}

// 多线程吞吐量
static void bench_multi_thread(int nThreads) {
    std::cout << "\n=== Benchmark 2: Multi-thread throughput (" << nThreads << " threads) ===\n";
    const std::string f = "/tmp/bronx_bench_multi_" + std::to_string(nThreads) + ".log";
    resetFile(f);
    auto lg = makeAsyncLogger("bench2_" + std::to_string(nThreads), f);

    constexpr int N_PER_THREAD = 100000;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> workers;
    auto t0 = Clock::now();

    for (int i = 0; i < nThreads; ++i) {
        workers.emplace_back([&, i]{
            ready.fetch_add(1);
            while (!start.load(std::memory_order_acquire)) {}

            for (int j = 0; j < N_PER_THREAD; ++j) {
                BRONX_LOG_INFO(lg) << "BxThread " << i << " message " << j << " payload";
            }
        });
    }

    while (ready.load() < nThreads) {}
    start.store(true, std::memory_order_release);

    for (auto& w : workers) w.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();
    auto t1 = Clock::now();

    int total = nThreads * N_PER_THREAD;
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    double throughput = total / elapsed;
    std::cout << "  Threads: " << nThreads << "\n";
    std::cout << "  Messages: " << total << "\n";
    std::cout << "  Time: " << elapsed << " s\n";
    std::cout << "  Throughput: " << static_cast<int>(throughput) << " msg/s\n";
    finishFile(lg, f);
}

// 延迟分布 (单线程)
static void bench_latency() {
    std::cout << "\n=== Benchmark 3: Latency distribution (single-thread) ===\n";
    const std::string f = "/tmp/bronx_bench_latency.log";
    resetFile(f);
    auto lg = makeAsyncLogger("bench3", f);

    constexpr int N = 50000;
    std::vector<int64_t> latencies;
    latencies.reserve(N);

    // warmup
    for (int i = 0; i < 1000; ++i) {
        BRONX_LOG_INFO(lg) << "warmup";
    }
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    for (int i = 0; i < N; ++i) {
        auto t0 = Clock::now();
        BRONX_LOG_INFO(lg) << "Latency measurement message #" << i;
        auto t1 = Clock::now();
        latencies.push_back(std::chrono::duration_cast<Ns>(t1 - t0).count());
    }
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    std::sort(latencies.begin(), latencies.end());
    auto p50 = latencies[N * 50 / 100];
    auto p95 = latencies[N * 95 / 100];
    auto p99 = latencies[N * 99 / 100];
    auto p999 = latencies[N * 999 / 1000];
    auto pmax = latencies.back();

    int64_t sum = 0;
    for (auto l : latencies) sum += l;
    auto avg = sum / N;

    std::cout << "  Samples: " << N << "\n";
    std::cout << "  Average: " << avg << " ns\n";
    std::cout << "  p50:     " << p50 << " ns\n";
    std::cout << "  p95:     " << p95 << " ns\n";
    std::cout << "  p99:     " << p99 << " ns\n";
    std::cout << "  p99.9:   " << p999 << " ns\n";
    std::cout << "  max:     " << pmax << " ns\n";
    finishFile(lg, f);
}

// 队列满压力 (缩小容量)
static void bench_backpressure() {
    std::cout << "\n=== Benchmark 4: Back-pressure (small queue) ===\n";

    // 临时缩小容量
    size_t oldBuf = BxAsyncLogger::kBufferCapacity;
    size_t oldQueue = BxAsyncLogger::kCentralCapacity;
    BxAsyncLogger::kBufferCapacity = 128;
    BxAsyncLogger::kCentralCapacity = 16;

    const std::string f = "/tmp/bronx_bench_pressure.log";
    resetFile(f);
    auto lg = makeAsyncLogger("bench4", f);

    constexpr int N_THREADS = 8;
    constexpr int N_PER = 10000;

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    auto t0 = Clock::now();

    for (int i = 0; i < N_THREADS; ++i) {
        workers.emplace_back([&, i]{
            while (!start.load()) {}
            for (int j = 0; j < N_PER; ++j) {
                BRONX_LOG_INFO(lg) << "Pressure thread " << i << " msg " << j;
            }
        });
    }

    start.store(true);
    for (auto& w : workers) w.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();
    auto t1 = Clock::now();

    int total = N_THREADS * N_PER;
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    double throughput = total / elapsed;
    std::cout << "  Queue capacity: " << BxAsyncLogger::kCentralCapacity << "\n";
    std::cout << "  Buffer capacity: " << BxAsyncLogger::kBufferCapacity << "\n";
    std::cout << "  Threads: " << N_THREADS << "\n";
    std::cout << "  Messages: " << total << "\n";
    std::cout << "  Time: " << elapsed << " s\n";
    std::cout << "  Throughput: " << static_cast<int>(throughput) << " msg/s\n";

    // 恢复
    BxAsyncLogger::kBufferCapacity = oldBuf;
    BxAsyncLogger::kCentralCapacity = oldQueue;
    finishFile(lg, f);
}

// 测试 shutdown 等待 in-flight 的正确性
static void bench_shutdown_inflight() {
    std::cout << "\n=== Benchmark 5: Shutdown with in-flight producers ===\n";
    const std::string f = "/tmp/bronx_bench_shutdown.log";
    resetFile(f);
    auto lg = makeAsyncLogger("bench5", f);

    constexpr int N_THREADS = 4;
    constexpr int N_PER = 50000;
    std::atomic<int> started{0};
    std::vector<std::thread> workers;

    auto t0 = Clock::now();
    for (int i = 0; i < N_THREADS; ++i) {
        workers.emplace_back([&, i]{
            started.fetch_add(1);
            for (int j = 0; j < N_PER; ++j) {
                BRONX_LOG_INFO(lg) << "Shutdown test thread " << i << " msg " << j;
            }
        });
    }

    // 等部分线程写到一半
    while (started.load() < N_THREADS) {}
    std::this_thread::sleep_for(Ms(10));

    // 模拟提前 shutdown
    auto ts = Clock::now();
    BxAsyncLoggerMgr::GetInstance()->shutdown();
    auto te = Clock::now();

    for (auto& w : workers) w.join();
    auto t1 = Clock::now();

    double total_time = std::chrono::duration<double>(t1 - t0).count();
    double shutdown_time = std::chrono::duration<double>(te - ts).count();

    // 验证文件行数
    std::ifstream ifs(f);
    int lines = 0;
    std::string line;
    while (std::getline(ifs, line)) ++lines;

    std::cout << "  Threads: " << N_THREADS << "\n";
    std::cout << "  Expected lines: " << N_THREADS * N_PER << "\n";
    std::cout << "  Actual lines: " << lines << "\n";
    std::cout << "  Total time: " << total_time << " s\n";
    std::cout << "  Shutdown wait: " << shutdown_time << " s\n";
    bool passed = lines == N_THREADS * N_PER;
    std::cout << "  Status: " << (passed ? "PASS ✓" : "FAIL ✗") << "\n";
    if(!passed) ++g_failed;
    finishFile(lg, f);
}

int main() {
    std::cout << "Async BxLogger Performance Benchmark\n";
    std::cout << "===================================\n";

    bench_single_thread();
    bench_multi_thread(2);
    bench_multi_thread(4);
    bench_multi_thread(8);
    bench_latency();
    bench_backpressure();
    bench_shutdown_inflight();

    std::cout << "\n=== All benchmarks completed ===\n";
    return g_failed == 0 ? 0 : 1;
}
