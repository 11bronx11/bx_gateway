#include "crash.h"

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/syscall.h>

namespace bronx {

namespace {

// 崩溃文件绝对路径, 装 handler 时拷进来。handler 里只读, 不碰堆。
char g_crashPath[4096] = {0};
// altstack 内存, 全局持有防栈溢出时没栈可用
char g_altStack[65536];
bool g_installed = false;

// --- write/clock_gettime/getpid/syscall 是 POSIX async-signal-safe; ---
// --- safeWrite/safeUint/safeHex/sigName 只用栈缓冲+write, 同等安全      ---

// 原样写, 忽略 EINTR。write 是 async-signal-safe。
void safeWrite(int fd, const char* s, size_t n) {
    while(n > 0) {
        ssize_t w = ::write(fd, s, n);
        if(w <= 0) {
            if(w < 0 && errno == EINTR) continue;
            break;
        }
        s += w; n -= (size_t)w;
    }
}

void safeStr(int fd, const char* s) { safeWrite(fd, s, strlen(s)); }

// 无符号整数转十进制写入 fd, 不用 snprintf(非严格 async-safe)
void safeUint(int fd, unsigned long v) {
    char buf[24];
    int i = sizeof(buf);
    buf[--i] = '\0';
    if(v == 0) buf[--i] = '0';
    while(v > 0 && i > 0) { buf[--i] = char('0' + v % 10); v /= 10; }
    safeStr(fd, buf + i);
}

// 指针转 0x 十六进制
void safeHex(int fd, unsigned long v) {
    static const char* hx = "0123456789abcdef";
    char buf[20];
    int i = sizeof(buf);
    buf[--i] = '\0';
    if(v == 0) buf[--i] = '0';
    while(v > 0 && i > 0) { buf[--i] = hx[v & 0xf]; v >>= 4; }
    safeStr(fd, "0x");
    safeStr(fd, buf + i);
}

const char* sigName(int sig) {
    switch(sig) {
        case SIGSEGV: return "SIGSEGV (segfault)";
        case SIGABRT: return "SIGABRT (abort/terminate)";
        case SIGBUS:  return "SIGBUS (bus error)";
        case SIGFPE:  return "SIGFPE (fp exception)";
        case SIGILL:  return "SIGILL (illegal insn)";
        default:      return "UNKNOWN";
    }
}

// 崩溃现场落地: 写一份到 crash 文件(追加), 一份到 STDERR。
void dump(int fd, int sig, siginfo_t* info) {
    safeStr(fd, "\n===== BRONX CRASH =====\n");
    // 时间戳 (epoch 秒, clock_gettime 是 async-signal-safe)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    safeStr(fd, "epoch_s="); safeUint(fd, (unsigned long)ts.tv_sec); safeStr(fd, "\n");
    safeStr(fd, "signal="); safeStr(fd, sigName(sig));
    safeStr(fd, " ("); safeUint(fd, (unsigned long)sig); safeStr(fd, ")\n");
    safeStr(fd, "pid="); safeUint(fd, (unsigned long)getpid()); safeStr(fd, "\n");
    safeStr(fd, "tid="); safeUint(fd, (unsigned long)syscall(SYS_gettid)); safeStr(fd, "\n");
    if(info) {
        safeStr(fd, "fault_addr="); safeHex(fd, (unsigned long)info->si_addr); safeStr(fd, "\n");
    }
    safeStr(fd, "backtrace:\n");
    void* frames[64];
    // backtrace: 非严格 POSIX async-signal-safe, 但已在 installCrashHandler 预热 dlopen;
    // backtrace_symbols_fd: glibc 实现不 malloc 直写 fd, 实践安全但同样非 POSIX 保证
    int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, fd);
    safeStr(fd, "===== END =====\n");
}

void handler(int sig, siginfo_t* info, void*) {
    // 打到 crash 文件 (追加, 0644)
    if(g_crashPath[0]) {
        int fd = ::open(g_crashPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if(fd >= 0) { dump(fd, sig, info); ::close(fd); }
    }
    // 再打一份到 STDERR
    dump(STDERR_FILENO, sig, info);

    // 恢复默认 handler 重新 raise, 保留原退出码 / core dump 行为
    signal(sig, SIG_DFL);
    raise(sig);
}

} // namespace

bool installCrashHandler(const char* crashFile) {
    if(g_installed) return true;
    g_installed = true;

    if(crashFile) {
        strncpy(g_crashPath, crashFile, sizeof(g_crashPath) - 1);
        g_crashPath[sizeof(g_crashPath) - 1] = '\0';
    }

    // 预热 backtrace: 首次调用会 dlopen libgcc, 崩溃现场里做不安全, 提前触发。
    void* warm[4];
    (void)::backtrace(warm, 4);

    // 独立信号栈: 栈溢出型 SIGSEGV 时主栈已废, 得有备用栈才能跑 handler。
    bool altOk = false;
    stack_t ss;
    ss.ss_sp = g_altStack;
    ss.ss_size = sizeof(g_altStack);
    ss.ss_flags = 0;
    if(sigaltstack(&ss, nullptr) == 0) altOk = true;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sigemptyset(&sa.sa_mask);
    // SA_SIGINFO 拿 fault_addr; SA_ONSTACK 用 altstack; SA_RESETHAND 双重保险防 handler 内再崩死循环。
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND | (altOk ? SA_ONSTACK : 0);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);

    return altOk;
}

} // namespace bronx
