// test_crash_handler.cpp — 崩溃 handler 验证
// ① fork 子进程 raise SIGABRT, 检查 crash 文件写入了期望字段
// ② BxThread 里读回 sigaltstack, 确认 per-thread altstack 已安装
#include "crash.h"
#include "thread.h"
#include "test_util.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <atomic>

static std::string readFile(const char* path) {
    int fd = open(path, O_RDONLY);
    if(fd < 0) return "";
    char buf[8192] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 ? std::string(buf, (size_t)n) : "";
}

// 装 handler → fork → 子进程 raise SIGABRT → 父进程检查 crash 文件内容
static void test_crash_writes_file() {
    const char* tmp = "/tmp/bronx_test_crash.log";
    unlink(tmp);

    pid_t pid = fork();
    if(pid == 0) {
        bronx::installCrashHandler(tmp);
        raise(SIGABRT);
        _exit(0);
    }
    TEST_CHECK_MSG(pid > 0, "fork should succeed");

    int st = 0;
    waitpid(pid, &st, 0);
    TEST_CHECK_MSG(WIFSIGNALED(st),          "child should die on signal");
    TEST_CHECK_MSG(WTERMSIG(st) == SIGABRT,  "child should die on SIGABRT");

    std::string s = readFile(tmp);
    TEST_CHECK_MSG(!s.empty(),                                  "crash.log must be non-empty");
    TEST_CHECK_MSG(s.find("BRONX CRASH")  != std::string::npos, "crash.log: header present");
    TEST_CHECK_MSG(s.find("SIGABRT")      != std::string::npos, "crash.log: signal name present");
    TEST_CHECK_MSG(s.find("backtrace")    != std::string::npos, "crash.log: backtrace section present");
    TEST_CHECK_MSG(s.find("pid=")         != std::string::npos, "crash.log: pid field present");

    unlink(tmp);
}

// BxThread::run 应已为本线程安装独立 altstack
static void test_worker_thread_has_altstack() {
    std::atomic<bool> hasAlt{false};
    bronx::BxThread t([&]{
        stack_t old{};
        // sigaltstack(nullptr, &old): 只读当前设置, 不修改
        if(sigaltstack(nullptr, &old) == 0) {
            hasAlt.store(old.ss_sp != nullptr && !(old.ss_flags & SS_DISABLE));
        }
    }, "altstack-check");
    t.join();
    TEST_CHECK_MSG(hasAlt.load(), "worker thread should have per-thread altstack installed");
}

int main() {
    test_crash_writes_file();
    test_worker_thread_has_altstack();
    return TEST_SUMMARY();
}
