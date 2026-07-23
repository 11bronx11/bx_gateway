// 异步日志测试
// 覆盖：
//   T1 单线程 N 条 INFO，shutdown 后行数 == N
//   T2 多线程 4*M 条，行数 == 4*M
//   T3 心跳：写一条后 sleep > 心跳周期，确认日志已落盘
//   T4 FATAL 落盘：FATAL 出现在文件最后，前面 INFO 全部已写入
//   T5 队列满压力：缩小容量 + 多线程狂打，不死锁、计数正确
//   T6 appender 生命周期：clearAppenders 后，异步队列中的旧 appender 仍安全写完
//   T7 root fallback：子 logger 使用 root appender 时，root 清空后旧日志仍安全写完

#include "async_logger.h"
#include "log.h"
#include "thread.h"
#include "config.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <vector>

static int countLines(const std::string& path) {
    std::ifstream ifs(path);
    int n = 0;
    std::string line;
    while (std::getline(ifs, line)) ++n;
    return n;
}

static std::string readAll(const std::string& path) {
    std::ifstream ifs(path);
    std::ostringstream os;
    os << ifs.rdbuf();
    return os.str();
}

static void resetFile(const std::string& path) {
    std::ofstream ofs(path, std::ios::trunc);
}

static int g_failed = 0;
#define EXPECT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << msg << " at line " << __LINE__ << "\n"; \
            ++g_failed; \
        } else { \
            std::cout << "[ OK ] " << msg << "\n"; \
        } \
    } while (0)


static bronx::BxLogger::ptr makeAsyncFileLogger(const std::string& name, const std::string& file) {
    auto logger = bronx::LoggerMgr::GetInstance()->getLogger(name);
    logger->clearAppenders();
    logger->setLevel(bronx::BxLogLevel::DEBUG);
    auto fa = std::make_shared<bronx::BxFileLogAppender>(file);
    fa->setAsync(true);
    fa->setFormatter(std::make_shared<bronx::BxLogFormatter>("%m%n"));
    logger->addAppender(fa);
    return logger;
}

static void test_single_thread() {
    const std::string f = "/tmp/bronx_async_t1.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("t1", f);
    constexpr int N = 10000;
    for (int i = 0; i < N; ++i) {
        BRONX_LOG_INFO(lg) << "line-" << i;
    }
    bronx::BxAsyncLoggerMgr::GetInstance()->flushAll();
    int n = countLines(f);
    EXPECT(n == N, "T1 single-thread line count = " << n << " expected " << N);
}

static void test_multi_thread() {
    const std::string f = "/tmp/bronx_async_t2.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("t2", f);
    constexpr int M = 50000;
    constexpr int T = 4;
    std::vector<bronx::BxThread::ptr> ths;
    for (int t = 0; t < T; ++t) {
        ths.emplace_back(new bronx::BxThread([lg, t]{
            for (int i = 0; i < M; ++i) {
                BRONX_LOG_INFO(lg) << "tid=" << t << " i=" << i;
            }
        }, "worker"));
    }
    for (auto& th : ths) th->join();
    bronx::BxAsyncLoggerMgr::GetInstance()->flushAll();
    int n = countLines(f);
    EXPECT(n == T * M, "T2 multi-thread line count = " << n << " expected " << T * M);
}

static void test_heartbeat() {
    // 心跳周期由 main 入口调小为 100ms。这里 sleep 350ms：足够让心跳触发 + writer
    // 写盘，又远小于原来 4.7 秒方案，CI 抖动也不易把它带 flaky。
    const std::string f = "/tmp/bronx_async_t3.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("t3", f);

    BRONX_LOG_INFO(lg) << "stale-line";
    // 不调用 flushAll，等心跳触发
    std::this_thread::sleep_for(std::chrono::milliseconds(350));

    int n = countLines(f);
    EXPECT(n == 1, "T3 heartbeat flush; line count = " << n << " expected 1");
}

static void test_fatal_flush() {
    const std::string f = "/tmp/bronx_async_t4.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("t4", f);
    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        BRONX_LOG_INFO(lg) << "info-" << i;
    }
    BRONX_LOG_FATAL(lg) << "FATAL_HERE";

    // FATAL 宏在调用点结束时已自动 flushAll；这里再次 flushAll 保证 stable
    bronx::BxAsyncLoggerMgr::GetInstance()->flushAll();

    auto contents = readAll(f);
    int lineCount = 0;
    for (char c : contents) if (c == '\n') ++lineCount;
    EXPECT(lineCount == N + 1, "T4 fatal+info lines = " << lineCount);

    // FATAL 必须出现在文件中
    EXPECT(contents.find("FATAL_HERE") != std::string::npos, "T4 FATAL_HERE present");
    // FATAL 行应在最后
    auto pos = contents.rfind("FATAL_HERE");
    EXPECT(pos != std::string::npos && pos > contents.find("info-0"),
           "T4 FATAL after INFO lines");
}

static void test_queue_pressure() {
    const std::string f = "/tmp/bronx_async_t5.log";
    resetFile(f);
    auto lg = makeAsyncFileLogger("t5", f);
    constexpr int M = 20000;
    constexpr int T = 4;
    std::vector<bronx::BxThread::ptr> ths;
    for (int t = 0; t < T; ++t) {
        ths.emplace_back(new bronx::BxThread([lg, t]{
            for (int i = 0; i < M; ++i) {
                BRONX_LOG_INFO(lg) << "pressure tid=" << t << " i=" << i;
            }
        }, "pressure"));
    }
    for (auto& th : ths) th->join();
    bronx::BxAsyncLoggerMgr::GetInstance()->flushAll();
    int n = countLines(f);
    EXPECT(n == T * M, "T5 pressure line count = " << n << " expected " << T * M);
}

static void test_appender_lifetime_after_clear() {
    const std::string f = "/tmp/bronx_async_t6.log";
    resetFile(f);
    auto logger = bronx::LoggerMgr::GetInstance()->getLogger("t6");
    logger->clearAppenders();
    logger->setLevel(bronx::BxLogLevel::DEBUG);

    auto fa = std::make_shared<bronx::BxFileLogAppender>(f);
    fa->setAsync(true);
    fa->setFormatter(std::make_shared<bronx::BxLogFormatter>("%m%n"));
    logger->addAppender(fa);

    constexpr int N = 5000;
    for (int i = 0; i < N; ++i) {
        BRONX_LOG_INFO(logger) << "lifetime-" << i;
    }

    // 回归点：队列项必须持有 appender 的 shared_ptr。
    // 这里清掉 logger 并释放本地引用后，pending buffer 仍应能安全写完。
    logger->clearAppenders();
    fa.reset();

    bronx::BxAsyncLoggerMgr::GetInstance()->flushAll();
    int n = countLines(f);
    EXPECT(n == N, "T6 appender lifetime line count = " << n << " expected " << N);
}

static void test_root_fallback_lifetime() {
    const std::string f = "/tmp/bronx_async_t7.log";
    resetFile(f);

    auto root = bronx::LoggerMgr::GetInstance()->getRoot();
    root->clearAppenders();
    root->setLevel(bronx::BxLogLevel::DEBUG);
    auto fa = std::make_shared<bronx::BxFileLogAppender>(f);
    fa->setAsync(true);
    fa->setFormatter(std::make_shared<bronx::BxLogFormatter>("%m%n"));
    root->addAppender(fa);

    auto child = bronx::LoggerMgr::GetInstance()->getLogger("t7_child");
    child->clearAppenders();
    child->setLevel(bronx::BxLogLevel::DEBUG);

    constexpr int N = 3000;
    for (int i = 0; i < N; ++i) {
        BRONX_LOG_INFO(child) << "root-fallback-" << i;
    }

    // 回归点：BxLogger::log() 需要复制 root appender 快照，且异步队列保留 shared_ptr。
    root->clearAppenders();
    fa.reset();

    bronx::BxAsyncLoggerMgr::GetInstance()->flushAll();
    int n = countLines(f);
    EXPECT(n == N, "T7 root fallback lifetime line count = " << n << " expected " << N);
}

static void test_file_appender_creates_parent() {
    namespace fs = std::filesystem;
    fs::path dir = fs::path("/tmp") / ("bronx_async_parent_" + std::to_string(getpid()));
    fs::path file = dir / "nested" / "log.txt";
    fs::remove_all(dir);

    auto logger = bronx::LoggerMgr::GetInstance()->getLogger("t8");
    logger->clearAppenders();
    logger->setLevel(bronx::BxLogLevel::DEBUG);
    auto fa = std::make_shared<bronx::BxFileLogAppender>(file.string());
    fa->setAsync(false);
    fa->setFormatter(std::make_shared<bronx::BxLogFormatter>("%m%n"));
    logger->addAppender(fa);

    BRONX_LOG_INFO(logger) << "parent-created";
    EXPECT(countLines(file.string()) == 1, "T8 file appender creates parent directory");

    logger->clearAppenders();
    fa.reset();
    fs::remove_all(dir);
}

int main(int argc, char** argv) {
    // 在 BxAsyncLogger 单例首次构造前调小容量和心跳：
    // 小容量能覆盖中心队列背压路径，短心跳让测试不必长时间等待。
    bronx::BxAsyncLogger::kBufferCapacity = 16;
    bronx::BxAsyncLogger::kCentralCapacity = 8;
    bronx::BxAsyncLogger::kHeartbeatMs = 100;
    // 关闭 root logger 默认 stdout，避免控制台太吵
    bronx::LoggerMgr::GetInstance()->getRoot()->setLevel(bronx::BxLogLevel::FATAL);

    test_single_thread();
    test_multi_thread();
    test_heartbeat();
    test_fatal_flush();
    test_queue_pressure();
    test_appender_lifetime_after_clear();
    test_root_fallback_lifetime();
    test_file_appender_creates_parent();
                 
    if (g_failed) {
        std::cerr << "\n[" << g_failed << " test(s) failed]\n";
        return 1;
    }
    std::cout << "\nAll async log tests passed\n";
    return 0;
}
