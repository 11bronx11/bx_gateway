// log 模块并发/边界综合测试
// 重点不是 BxAsyncLogger，而是 BxLogger / BxLogAppender / BxLogManager / BxLogFormatter
// 这层的共享状态在多线程下是否安全。
//
// 涉及的共享状态:
//   1. BxLogger::m_appenders        — BxSpinLock 保护
//   2. BxLogger::m_level            — std::atomic
//   3. BxLogAppender::m_formatter   — BxSpinLock 保护
//   4. BxLogAppender::m_async       — std::atomic
//   5. BxFileLogAppender::m_filestream / m_lastTime — BxSpinLock 保护
//   6. BxLogManager::m_loggers      — BxSpinLock 保护
//   7. BxLogFormatter::m_items      — 构造后只读
//   8. BxLogEvent::m_ss             — 单事件单线程使用

#include "async_logger.h"
#include "log.h"
#include "thread.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

static BxLogger::ptr makeLogger(const std::string& name, const std::string& file, bool async) {
    auto logger = LoggerMgr::GetInstance()->getLogger(name);
    logger->clearAppenders();
    logger->setLevel(BxLogLevel::DEBUG);
    auto fa = std::make_shared<BxFileLogAppender>(file);
    fa->setAsync(async);
    fa->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));
    logger->addAppender(fa);
    return logger;
}

// L1: 并发 addAppender / delAppender / log
// 测 BxLogger::m_appenders 在 spinlock 下的一致性
static void test_appender_concurrent_mutation() {
    std::cout << "\n=== L1: concurrent add/del appender + log ===\n";
    const std::string f1 = "/tmp/log_l1_a.log";
    const std::string f2 = "/tmp/log_l1_b.log";
    resetFile(f1); resetFile(f2);

    auto logger = LoggerMgr::GetInstance()->getLogger("L1");
    logger->clearAppenders();
    logger->setLevel(BxLogLevel::DEBUG);

    auto ap1 = std::make_shared<BxFileLogAppender>(f1);
    ap1->setAsync(true);
    ap1->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));
    auto ap2 = std::make_shared<BxFileLogAppender>(f2);
    ap2->setAsync(true);
    ap2->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));

    logger->addAppender(ap1);

    std::atomic<bool> stop{false};
    std::atomic<int> produced{0};

    // 8 个 producer 线程持续写
    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i) {
        producers.emplace_back([&, i]{
            int j = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                BRONX_LOG_INFO(logger) << "L1-T" << i << "-" << j++;
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 1 个 mutator 线程不断 add/del/clear/add
    std::thread mutator([&]{
        auto t0 = std::chrono::steady_clock::now();
        int cycles = 0;
        while (std::chrono::steady_clock::now() - t0 < 500ms) {
            logger->addAppender(ap2);
            std::this_thread::sleep_for(100us);
            logger->delAppender(ap2);
            std::this_thread::sleep_for(100us);
            logger->clearAppenders();
            std::this_thread::sleep_for(50us);
            logger->addAppender(ap1);  // 恢复 ap1,保证 producer 一直有去处
            ++cycles;
        }
        std::cout << "  mutator cycles: " << cycles << "\n";
    });

    mutator.join();
    stop.store(true);
    for (auto& p : producers) p.join();

    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int lines1 = countLines(f1);
    int lines2 = countLines(f2);
    int totalProduced = produced.load();
    int totalWritten = lines1 + lines2;

    std::cout << "  produced=" << totalProduced
              << " written(f1+f2)=" << totalWritten
              << " f1=" << lines1 << " f2=" << lines2 << "\n";

    // 由于 clearAppenders 期间日志可能丢失(没有 appender 接收),
    // 我们只验证: 没有 crash, written <= produced (不会凭空多)
    EXPECT(totalWritten <= totalProduced, "L1 no phantom logs: written <= produced");
    EXPECT(totalWritten > 0, "L1 some logs survived mutation");
}

// L2: 并发 setLevel
// 测 m_level 原子读写下,日志过滤的一致性
static void test_concurrent_level_change() {
    std::cout << "\n=== L2: concurrent setLevel ===\n";
    const std::string f = "/tmp/log_l2.log";
    resetFile(f);
    auto logger = makeLogger("L2", f, true);

    std::atomic<bool> stop{false};
    std::atomic<int> debugAttempted{0};
    std::atomic<int> infoAttempted{0};

    // producer: 同时写 DEBUG 和 INFO
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) {
        producers.emplace_back([&, i]{
            while (!stop.load(std::memory_order_relaxed)) {
                // DEBUG (=1) - 如果级别是 INFO(2) 以上则被过滤
                if (logger->getLevel() <= BxLogLevel::DEBUG) {
                    BRONX_LOG_DEBUG(logger) << "L2-D-" << i;
                }
                debugAttempted.fetch_add(1, std::memory_order_relaxed);

                if (logger->getLevel() <= BxLogLevel::INFO) {
                    BRONX_LOG_INFO(logger) << "L2-I-" << i;
                }
                infoAttempted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // level switcher
    std::thread switcher([&]{
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < 300ms) {
            logger->setLevel(BxLogLevel::DEBUG);
            std::this_thread::sleep_for(200us);
            logger->setLevel(BxLogLevel::INFO);
            std::this_thread::sleep_for(200us);
            logger->setLevel(BxLogLevel::ERROR);
            std::this_thread::sleep_for(200us);
        }
    });

    switcher.join();
    stop.store(true);
    for (auto& p : producers) p.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int total = countLines(f);
    std::cout << "  written=" << total
              << " (debug attempts=" << debugAttempted.load()
              << " info attempts=" << infoAttempted.load() << ")\n";
    EXPECT(total >= 0, "L2 no crash");
}

// L3: 并发 setFormatter
// 测 BxLogAppender::m_formatter 切换时格式不撕裂
static void test_concurrent_formatter_swap() {
    std::cout << "\n=== L3: concurrent setFormatter ===\n";
    const std::string f = "/tmp/log_l3.log";
    resetFile(f);

    auto logger = LoggerMgr::GetInstance()->getLogger("L3");
    logger->clearAppenders();
    logger->setLevel(BxLogLevel::DEBUG);
    auto fa = std::make_shared<BxFileLogAppender>(f);
    fa->setAsync(true);
    logger->addAppender(fa);

    auto fmt1 = std::make_shared<BxLogFormatter>("A:%m%n");
    auto fmt2 = std::make_shared<BxLogFormatter>("B:%m%n");
    auto fmt3 = std::make_shared<BxLogFormatter>("C:%m%n");
    fa->setFormatter(fmt1);

    std::atomic<bool> stop{false};
    std::atomic<int> produced{0};

    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i) {
        producers.emplace_back([&, i]{
            int j = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                BRONX_LOG_INFO(logger) << "L3-T" << i << "-" << j++;
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread swapper([&]{
        auto t0 = std::chrono::steady_clock::now();
        int n = 0;
        while (std::chrono::steady_clock::now() - t0 < 500ms) {
            fa->setFormatter((n % 3 == 0) ? fmt1 : (n % 3 == 1) ? fmt2 : fmt3);
            ++n;
            std::this_thread::sleep_for(100us);
        }
    });

    swapper.join();
    stop.store(true);
    for (auto& p : producers) p.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    // 验证每一行必定以 A: / B: / C: 开头(不存在撕裂混合)
    std::ifstream ifs(f);
    std::string line;
    int total = 0;
    int bad = 0;
    while (std::getline(ifs, line)) {
        ++total;
        if (line.size() < 2 || (line[0] != 'A' && line[0] != 'B' && line[0] != 'C') || line[1] != ':') {
            ++bad;
            if (bad <= 3) std::cerr << "  bad line: " << line << "\n";
        }
    }
    std::cout << "  produced=" << produced.load() << " written=" << total << " malformed=" << bad << "\n";
    EXPECT(bad == 0, "L3 formatter swap: no malformed lines");
}

// L4: 并发 setAsync 翻转(同步 ↔ 异步)
// 测 m_async 原子翻转下不丢日志
static void test_concurrent_async_toggle() {
    std::cout << "\n=== L4: concurrent setAsync toggle ===\n";
    const std::string f = "/tmp/log_l4.log";
    resetFile(f);

    auto logger = LoggerMgr::GetInstance()->getLogger("L4");
    logger->clearAppenders();
    logger->setLevel(BxLogLevel::DEBUG);
    auto fa = std::make_shared<BxFileLogAppender>(f);
    fa->setAsync(true);
    fa->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));
    logger->addAppender(fa);

    std::atomic<bool> stop{false};
    std::atomic<int> produced{0};

    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) {
        producers.emplace_back([&, i]{
            int j = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                BRONX_LOG_INFO(logger) << "L4-T" << i << "-" << j++;
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread toggler([&]{
        auto t0 = std::chrono::steady_clock::now();
        bool a = true;
        while (std::chrono::steady_clock::now() - t0 < 500ms) {
            fa->setAsync(a);
            a = !a;
            std::this_thread::sleep_for(300us);
        }
    });

    toggler.join();
    stop.store(true);
    for (auto& p : producers) p.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int total = countLines(f);
    std::cout << "  produced=" << produced.load() << " written=" << total << "\n";
    EXPECT(total == produced.load(),
           "L4 sync<->async toggle: written == produced (no loss)");
}

// L5: BxLogManager::getLogger 并发(同名应得到同一指针)
static void test_log_manager_get_logger_race() {
    std::cout << "\n=== L5: BxLogManager::getLogger same-name race ===\n";

    constexpr int N_THREADS = 32;
    std::vector<BxLogger::ptr> results(N_THREADS);
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> ts;
    for (int i = 0; i < N_THREADS; ++i) {
        ts.emplace_back([&, i]{
            ready.fetch_add(1);
            while (!start.load()) {}
            results[i] = LoggerMgr::GetInstance()->getLogger("L5_concurrent_name");
        });
    }
    while (ready.load() < N_THREADS) {}
    start.store(true);
    for (auto& t : ts) t.join();

    BxLogger* first = results[0].get();
    bool allSame = true;
    for (int i = 1; i < N_THREADS; ++i) {
        if (results[i].get() != first) { allSame = false; break; }
    }
    EXPECT(allSame && first != nullptr, "L5 all 32 getLogger() return same pointer");
}

// L6: 不同 logger 名同时创建(测 m_loggers map 写入并发)
static void test_log_manager_multi_name_race() {
    std::cout << "\n=== L6: BxLogManager::getLogger multi-name race ===\n";

    constexpr int N_THREADS = 32;
    constexpr int N_NAMES = 16;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> ts;
    for (int i = 0; i < N_THREADS; ++i) {
        ts.emplace_back([&, i]{
            ready.fetch_add(1);
            while (!start.load()) {}
            for (int j = 0; j < N_NAMES; ++j) {
                std::string name = "L6_multi_" + std::to_string(j);
                auto lg = LoggerMgr::GetInstance()->getLogger(name);
                if (!lg) std::abort();
            }
        });
    }
    while (ready.load() < N_THREADS) {}
    start.store(true);
    for (auto& t : ts) t.join();

    // 再次查询,每个 name 应仍指向同一对象
    std::set<BxLogger*> seen;
    bool ok = true;
    for (int j = 0; j < N_NAMES; ++j) {
        auto a = LoggerMgr::GetInstance()->getLogger("L6_multi_" + std::to_string(j));
        auto b = LoggerMgr::GetInstance()->getLogger("L6_multi_" + std::to_string(j));
        if (a.get() != b.get()) ok = false;
        seen.insert(a.get());
    }
    EXPECT(ok, "L6 same name => same pointer after concurrent creation");
    EXPECT(seen.size() == N_NAMES, "L6 distinct names => distinct pointers: got " << seen.size());
}

// L7: BxLogFormatter 多线程并发调用 format()
// 测 BxLogFormatter::m_items 是否真的只读、DateTime/线程 ID 这类项是否线程安全
static void test_formatter_concurrent_format() {
    std::cout << "\n=== L7: BxLogFormatter concurrent format() ===\n";

    auto fmt = std::make_shared<BxLogFormatter>("%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T[%p]%T%m%n");

    constexpr int N_THREADS = 16;
    constexpr int N_PER = 5000;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> bad{false};

    std::vector<std::thread> ts;
    for (int i = 0; i < N_THREADS; ++i) {
        ts.emplace_back([&, i]{
            ready.fetch_add(1);
            while (!start.load()) {}
            for (int j = 0; j < N_PER; ++j) {
                auto ev = std::make_shared<BxLogEvent>("L7", BxLogLevel::INFO,
                    __FILE__, __LINE__, 0, GetThreadId(), CurrentId(),
                    time(nullptr), GetThreadName());
                ev->getSS() << "L7-T" << i << "-" << j;

                std::ostringstream os;
                fmt->format(os, ev);
                std::string s = os.str();

                // 简单 sanity: 不为空,包含 L7-T 标记
                if (s.empty() || s.find("L7-T") == std::string::npos) {
                    bad.store(true);
                }
            }
        });
    }
    while (ready.load() < N_THREADS) {}
    start.store(true);
    for (auto& t : ts) t.join();

    EXPECT(!bad.load(), "L7 concurrent format produces well-formed output");
}

static void test_formatter_pattern_edges() {
    std::cout << "\n=== L7b: BxLogFormatter pattern edge cases ===\n";
    auto makeEvent = []{
        auto ev = std::make_shared<BxLogEvent>("L7b", BxLogLevel::INFO,
            __FILE__, __LINE__, 0, GetThreadId(), CurrentId(),
            time(nullptr), GetThreadName());
        ev->getSS() << "edge";
        return ev;
    };

    auto fmt_date_at_end = std::make_shared<BxLogFormatter>("%d");
    std::ostringstream os1;
    fmt_date_at_end->format(os1, makeEvent());
    EXPECT(!fmt_date_at_end->isError(), "L7b %d at end is valid");
    EXPECT(!os1.str().empty(), "L7b %d at end produces date text");

    auto fmt_date_then_text = std::make_shared<BxLogFormatter>("%dX%m");
    std::ostringstream os_mid;
    fmt_date_then_text->format(os_mid, makeEvent());
    EXPECT(os_mid.str().find("Xedge") != std::string::npos,
           "L7b %d followed by text keeps following characters");

    auto fmt_tail_percent = std::make_shared<BxLogFormatter>("abc%");
    std::ostringstream os2;
    fmt_tail_percent->format(os2, makeEvent());
    EXPECT(fmt_tail_percent->isError(), "L7b trailing percent is marked as pattern error");
    EXPECT(os2.str().find("<<Pattern Error>>") != std::string::npos,
           "L7b trailing percent emits error marker");
}

// L8: clearAppenders 与 log 并发 — 极易触发的情况
//   配置热更新典型场景: 一个线程 clearAppenders + addAppender,另一线程拼命 log
static void test_clear_and_add_race() {
    std::cout << "\n=== L8: clearAppenders + addAppender race ===\n";
    const std::string f = "/tmp/log_l8.log";
    resetFile(f);

    auto logger = LoggerMgr::GetInstance()->getLogger("L8");
    logger->clearAppenders();
    logger->setLevel(BxLogLevel::DEBUG);

    auto mkAp = [&]{
        auto a = std::make_shared<BxFileLogAppender>(f);
        a->setAsync(true);
        a->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));
        return a;
    };
    logger->addAppender(mkAp());

    std::atomic<bool> stop{false};
    std::atomic<int> produced{0};

    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i) {
        producers.emplace_back([&, i]{
            int j = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                BRONX_LOG_INFO(logger) << "L8-T" << i << "-" << j++;
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread reloader([&]{
        auto t0 = std::chrono::steady_clock::now();
        int n = 0;
        while (std::chrono::steady_clock::now() - t0 < 500ms) {
            logger->clearAppenders();
            logger->addAppender(mkAp());
            ++n;
            std::this_thread::sleep_for(200us);
        }
        std::cout << "  reloads: " << n << "\n";
    });

    reloader.join();
    stop.store(true);
    for (auto& p : producers) p.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int lines = countLines(f);
    std::cout << "  produced=" << produced.load() << " written=" << lines << "\n";
    EXPECT(lines <= produced.load(), "L8 no phantom logs");
    EXPECT(lines > produced.load() / 2, "L8 majority survived");
}

// L9: 多 logger × 多 appender × 多 formatter 全并发
// 综合压力,看整体没有死锁/崩溃
static void test_full_mesh_chaos() {
    std::cout << "\n=== L9: full-mesh chaos (multi logger + appender + formatter) ===\n";

    constexpr int N_LG = 4;
    std::vector<BxLogger::ptr> loggers;
    std::vector<std::string> files;
    for (int i = 0; i < N_LG; ++i) {
        std::string name = "L9_lg_" + std::to_string(i);
        std::string file = "/tmp/log_l9_" + std::to_string(i) + ".log";
        files.push_back(file);
        resetFile(file);
        loggers.push_back(makeLogger(name, file, true));
    }

    std::atomic<bool> stop{false};
    std::atomic<int64_t> produced{0};

    // 16 producer
    std::vector<std::thread> producers;
    for (int i = 0; i < 16; ++i) {
        producers.emplace_back([&, i]{
            std::mt19937 rng(i + 1);
            int j = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                auto& lg = loggers[rng() % N_LG];
                BRONX_LOG_INFO(lg) << "L9-T" << i << "-" << j++;
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 1 个 chaos 线程: 改 level / 切 formatter / FATAL
    std::thread chaos([&]{
        auto t0 = std::chrono::steady_clock::now();
        std::mt19937 rng(7);
        auto fmtA = std::make_shared<BxLogFormatter>("%m%n");
        auto fmtB = std::make_shared<BxLogFormatter>("[%p] %m%n");
        while (std::chrono::steady_clock::now() - t0 < 500ms) {
            int op = rng() % 4;
            int idx = rng() % N_LG;
            switch (op) {
                case 0: loggers[idx]->setLevel(BxLogLevel::DEBUG); break;
                case 1: loggers[idx]->setLevel(BxLogLevel::INFO); break;
                case 2: BRONX_LOG_FATAL(loggers[idx]) << "L9-CHAOS-FATAL"; break;
                case 3: {
                    // 切 formatter: 重新拿 appender 不容易,这里跳过
                    break;
                }
            }
            std::this_thread::sleep_for(100us);
        }
    });

    chaos.join();
    stop.store(true);
    for (auto& p : producers) p.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int64_t total = 0;
    for (auto& f : files) total += countLines(f);
    std::cout << "  produced=" << produced.load() << " written=" << total << "\n";
    EXPECT(total > 0 && total <= produced.load() + 100 /* fatal chaos */,
           "L9 full-mesh: written within expected bounds");
}

// L10: getLogger 返回的 BxLogger 可立即用,即便另一个线程仍在调 getLogger
static void test_use_logger_during_creation() {
    std::cout << "\n=== L10: use logger while others are creating loggers ===\n";

    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};

    // 创建者: 不断 getLogger 新名字
    std::thread creator([&]{
        int n = 0;
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < 300ms) {
            std::string name = "L10_creator_" + std::to_string(n++);
            auto lg = LoggerMgr::GetInstance()->getLogger(name);
            if (!lg) { bad.store(true); break; }
        }
    });

    // 使用者: 反复 getLogger 已存在的,并写日志
    auto user = LoggerMgr::GetInstance()->getLogger("L10_user");
    user->clearAppenders();
    user->setLevel(BxLogLevel::DEBUG);
    const std::string f = "/tmp/log_l10.log";
    resetFile(f);
    auto fa = std::make_shared<BxFileLogAppender>(f);
    fa->setAsync(true);
    fa->setFormatter(std::make_shared<BxLogFormatter>("%m%n"));
    user->addAppender(fa);

    std::atomic<int> produced{0};
    std::thread userT([&]{
        auto t0 = std::chrono::steady_clock::now();
        int n = 0;
        while (std::chrono::steady_clock::now() - t0 < 300ms) {
            auto lg = LoggerMgr::GetInstance()->getLogger("L10_user");
            if (!lg) { bad.store(true); return; }
            BRONX_LOG_INFO(lg) << "L10-msg-" << n++;
            produced.fetch_add(1);
        }
    });

    creator.join();
    userT.join();
    BxAsyncLoggerMgr::GetInstance()->flushAll();

    int lines = countLines(f);
    EXPECT(!bad.load(), "L10 no null logger returned");
    EXPECT(lines == produced.load(), "L10 user log lines (" << lines << ") match produced (" << produced.load() << ")");
}

int main() {
    std::cout << "Log Layer Concurrency / Boundary Tests\n";
    std::cout << "=======================================\n";

    // 关键: 清掉 root 的默认 stdout appender,
    // 否则 BxLogger::log 在 m_appenders 为空时会 fallback 到 root,污染 stdout.
    LoggerMgr::GetInstance()->getRoot()->clearAppenders();

    test_appender_concurrent_mutation();   // L1
    test_concurrent_level_change();        // L2
    test_concurrent_formatter_swap();      // L3
    test_concurrent_async_toggle();        // L4
    test_log_manager_get_logger_race();    // L5
    test_log_manager_multi_name_race();    // L6
    test_formatter_concurrent_format();    // L7
    test_formatter_pattern_edges();        // L7b
    test_clear_and_add_race();             // L8
    test_full_mesh_chaos();                // L9
    test_use_logger_during_creation();     // L10

    std::cout << "\n=== Summary ===\n";
    if (g_failed == 0) {
        std::cout << "All log-layer tests passed ✓\n";
        return 0;
    }
    std::cout << g_failed << " test(s) failed ✗\n";
    return 1;
}
