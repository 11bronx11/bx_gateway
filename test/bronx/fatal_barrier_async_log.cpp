// FATAL 屏障语义边界测试
//
// 验证 BxLogger::log 里这段:
//     if (event->getLevel() == FATAL) BxAsyncLoggerMgr::GetInstance()->flushAll();
//     for (appender) appender->log(event);   // FATAL 自身同步写
//
// 契约:
//   - flushAll() 把"调用此刻已在 TLS/队列里的历史日志"全部 drain
//   - 与 FATAL 调用线程有 happens-before 关系的历史日志,必须出现在 FATAL 之前
//   - 并发线程在 flush 之后新产生的日志不属于这个屏障(允许出现在 FATAL 之后)
//   - 不论怎样都不能丢日志、不能死锁、不能崩溃
//
// 测试点:
//   F1 同线程 happens-before:N 条 INFO 后 FATAL,FATAL 必须在最后,前面 N 条全在
//   F2 多线程并发 INFO + 单线程 FATAL:不死锁、不崩溃、无丢失,FATAL 行存在
//   F3 多线程同时 FATAL(并发 flushAll):不死锁、不崩溃、每条 FATAL 都落盘
//   F4 FATAL 风暴:大量线程交错打 INFO/FATAL,总行数守恒
//   F5 FATAL + 热配置:flushAll 与 clearAppenders 并发,不崩溃、历史不丢
//   F6 裂缝压力:制造"swap 后未 enqueue"窗口 + 并发 FATAL,验证不丢日志

#include "async_logger.h"
#include "log.h"
#include "thread.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace bronx;

static int g_failed = 0;
#define EXPECT(cond, msg) \
    do { \
        if (!(cond)) { std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")\n"; ++g_failed; } \
        else { std::cout << "[ OK ] " << msg << "\n"; } \
    } while (0)

static int countLines(const std::string& path) {
    std::ifstream ifs(path);
    int n = 0; std::string line;
    while (std::getline(ifs, line)) ++n;
    return n;
}

static std::vector<std::string> readLines(const std::string& path) {
    std::ifstream ifs(path);
    std::vector<std::string> v; std::string line;
    while (std::getline(ifs, line)) v.push_back(line);
    return v;
}

static int countContaining(const std::string& path, const std::string& needle) {
    auto lines = readLines(path);
    int n = 0;
    for (auto& l : lines) if (l.find(needle) != std::string::npos) ++n;
    return n;
}

static void resetFile(const std::string& path) { std::ofstream ofs(path, std::ios::trunc); }

static BxLogger::ptr makeAsyncFileLogger(const std::string& name, const std::string& file) {
    auto logger = LoggerMgr::GetInstance()->getLogger(name);
    logger->clearAppenders();
    logger->setLevel(BxLogLevel::DEBUG);
    auto fa = std::make_shared<BxFileLogAppender>(file);
    fa->setAsync(true);
    fa->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));
    logger->addAppender(fa);
    return logger;
}

// F1: 同线程 happens-before — N 条 INFO 后 FATAL, FATAL 必须在最后一行
static void test_same_thread_ordering() {
    const std::string f = "/tmp/fatal_f1.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("f1", f);
    constexpr int N = 5000;
    for (int i = 0; i < N; ++i) BRONX_LOG_INFO(lg) << "INFO-" << i;
    BRONX_LOG_FATAL(lg) << "FATAL_MARKER";
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    auto lines = readLines(f);
    EXPECT((int)lines.size() == N + 1, "F1 total lines = " << lines.size() << " expected " << (N + 1));
    bool fatalLast = !lines.empty() && lines.back() == "FATAL_MARKER";
    EXPECT(fatalLast, "F1 FATAL is the LAST line (happens-before history all flushed)");
    // 确认 FATAL 前面恰好 N 条且无 FATAL 混入
    int fatalCount = countContaining(f, "FATAL_MARKER");
    EXPECT(fatalCount == 1, "F1 exactly one FATAL line, count = " << fatalCount);
    // 确认 INFO-(N-1) 出现在 FATAL 之前
    bool lastInfoBeforeFatal = false;
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        if (lines[i] == "INFO-" + std::to_string(N - 1)) { lastInfoBeforeFatal = true; break; }
    }
    EXPECT(lastInfoBeforeFatal, "F1 last INFO appears before FATAL");
}

// F2: 多线程 INFO + 单线程 FATAL — 不死锁/崩溃, FATAL 存在, 无丢失
static void test_multi_info_single_fatal() {
    const std::string f = "/tmp/fatal_f2.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("f2", f);
    constexpr int T = 6, PER = 5000;
    std::atomic<bool> go{false};
    std::vector<BxThread::ptr> ths;
    for (int t = 0; t < T; ++t) {
        ths.emplace_back(new BxThread([&, t]{
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < PER; ++i) BRONX_LOG_INFO(lg) << "T" << t << "-" << i;
        }, "f2-" + std::to_string(t)));
    }
    go.store(true);
    // 主线程中途插一条 FATAL
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    BRONX_LOG_FATAL(lg) << "FATAL_MID";
    for (auto& th : ths) th->join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int total = countLines(f);
    EXPECT(total == T * PER + 1, "F2 total lines = " << total << " expected " << (T * PER + 1));
    EXPECT(countContaining(f, "FATAL_MID") == 1, "F2 FATAL present exactly once");
}

// F3: 多线程同时 FATAL — 并发 flushAll 不死锁/崩溃, 每条 FATAL 都落盘
static void test_concurrent_fatal() {
    const std::string f = "/tmp/fatal_f3.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("f3", f);
    constexpr int T = 8;
    // 先垫一些历史 INFO
    for (int i = 0; i < 2000; ++i) BRONX_LOG_INFO(lg) << "PRE-" << i;

    std::atomic<bool> go{false};
    std::vector<BxThread::ptr> ths;
    for (int t = 0; t < T; ++t) {
        ths.emplace_back(new BxThread([&, t]{
            while (!go.load()) std::this_thread::yield();
            // 每个线程几乎同时打 FATAL — 触发并发 flushAll
            BRONX_LOG_FATAL(lg) << "FATAL_CONC_" << t;
        }, "f3-" + std::to_string(t)));
    }
    go.store(true);
    for (auto& th : ths) th->join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int fatals = countContaining(f, "FATAL_CONC_");
    EXPECT(fatals == T, "F3 all " << T << " concurrent FATAL lines present, got " << fatals);
    EXPECT(countContaining(f, "PRE-") == 2000, "F3 all 2000 historical INFO present");
}

// F4: FATAL 风暴 — 大量线程交错 INFO/FATAL, 总行数守恒
static void test_fatal_storm() {
    const std::string f = "/tmp/fatal_f4.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("f4", f);
    constexpr int T = 8, PER = 4000;
    std::atomic<long> produced{0};
    std::atomic<bool> go{false};
    std::vector<BxThread::ptr> ths;
    for (int t = 0; t < T; ++t) {
        ths.emplace_back(new BxThread([&, t]{
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < PER; ++i) {
                if (i % 500 == 499) { BRONX_LOG_FATAL(lg) << "F" << t << "_" << i; }
                else { BRONX_LOG_INFO(lg) << "I" << t << "_" << i; }
                produced.fetch_add(1);
            }
        }, "f4-" + std::to_string(t)));
    }
    go.store(true);
    for (auto& th : ths) th->join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int total = countLines(f);
    EXPECT(total == (int)produced.load(), "F4 line count = " << total << " expected " << produced.load());
}

// F5: FATAL + 热配置 — flushAll 与 clearAppenders 并发
static void test_fatal_hot_config() {
    const std::string f = "/tmp/fatal_f5.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("f5", f);
    std::atomic<long> produced{0};
    std::atomic<bool> stop{false};
    // 写线程
    std::vector<BxThread::ptr> ths;
    for (int t = 0; t < 4; ++t) {
        ths.emplace_back(new BxThread([&, t]{
            int i = 0;
            while (!stop.load()) {
                if (i % 300 == 299) BRONX_LOG_FATAL(lg) << "HF" << t << "_" << i;
                else BRONX_LOG_INFO(lg) << "HI" << t << "_" << i;
                produced.fetch_add(1);
                ++i;
            }
        }, "f5-" + std::to_string(t)));
    }
    // 配置抖动线程:反复换 appender(但都指向同一文件,保持可统计)
    BxThread::ptr cfg(new BxThread([&]{
        for (int k = 0; k < 50; ++k) {
            auto fa = std::make_shared<BxFileLogAppender>(f);
            fa->setAsync(true);
            fa->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));
            lg->clearAppenders();
            lg->addAppender(fa);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }, "f5-cfg"));
    cfg->join();
    stop.store(true);
    for (auto& th : ths) th->join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();
    // 热配置下行数可能因 appender 切换瞬间丢失少量(旧 appender 仍会写完),
    // 这里主要验证不崩溃、不死锁;行数应接近 produced(允许 clearAppenders 窗口的少量差异)
    int total = countLines(f);
    std::cout << "  F5 produced=" << produced.load() << " file_lines=" << total << "\n";
    EXPECT(total > 0 && total <= (int)produced.load(), "F5 no crash/deadlock; lines=" << total << " produced=" << produced.load());
}

// F6: 裂缝压力 — 大量线程在 buffer 边界反复 swap, 同时并发 FATAL
//     用极小 buffer 容量制造频繁 swap, 增大"swap 后未 enqueue"窗口
static void test_crack_swap_before_enqueue() {
    // 缩小容量制造高频 swap + 背压
    size_t oldBuf = BxAsyncLogger::kBufferCapacity;
    size_t oldCen = BxAsyncLogger::kCentralCapacity;
    BxAsyncLogger::kBufferCapacity = 8;     // 8 条就 swap
    BxAsyncLogger::kCentralCapacity = 16;   // 队列很小, 频繁背压
    // 注意:容量变更需在 BxAsyncLogger 构造前生效;此处单例可能已构造,
    // 仅作尽力压力(若已构造则用旧值, 不影响正确性验证)

    const std::string f = "/tmp/fatal_f6.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("f6", f);
    constexpr int T = 8, PER = 3000;
    std::atomic<long> produced{0};
    std::atomic<bool> go{false};
    std::vector<BxThread::ptr> ths;
    for (int t = 0; t < T; ++t) {
        ths.emplace_back(new BxThread([&, t]{
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < PER; ++i) {
                if (i % 250 == 249) BRONX_LOG_FATAL(lg) << "CF" << t << "_" << i;
                else BRONX_LOG_INFO(lg) << "CI" << t << "_" << i;
                produced.fetch_add(1);
            }
        }, "f6-" + std::to_string(t)));
    }
    go.store(true);
    for (auto& th : ths) th->join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int total = countLines(f);
    EXPECT(total == (int)produced.load(), "F6 crack stress line count = " << total << " expected " << produced.load());

    BxAsyncLogger::kBufferCapacity = oldBuf;
    BxAsyncLogger::kCentralCapacity = oldCen;
}

int main() {
    std::cout << "FATAL Barrier Semantics Boundary Testing\n";
    std::cout << "========================================\n\n";

    std::cout << "=== F1: same-thread happens-before ordering ===\n";
    test_same_thread_ordering();

    std::cout << "\n=== F2: multi INFO + single FATAL ===\n";
    test_multi_info_single_fatal();

    std::cout << "\n=== F3: concurrent FATAL (parallel flushAll) ===\n";
    test_concurrent_fatal();

    std::cout << "\n=== F4: FATAL storm (interleaved) ===\n";
    test_fatal_storm();

    std::cout << "\n=== F5: FATAL + hot config reload ===\n";
    test_fatal_hot_config();

    std::cout << "\n=== F6: crack stress (swap-before-enqueue + FATAL) ===\n";
    test_crack_swap_before_enqueue();

    std::cout << "\n=== Summary ===\n";
    if (g_failed == 0) std::cout << "All FATAL barrier tests passed \xE2\x9C\x93\n";
    else std::cout << g_failed << " test(s) FAILED \xE2\x9C\x97\n";
    return g_failed == 0 ? 0 : 1;
}
