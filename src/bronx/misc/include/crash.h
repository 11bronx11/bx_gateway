#pragma once

// 崩溃信号处理器: SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL 落地时打栈到独立 crash 文件。
// 框架异步日志崩溃时会丢尾, gdb 又要蹲守, 所以 handler 里尽量少做:
//   - write/clock_gettime/syscall(SYS_gettid)/getpid — POSIX async-signal-safe, 无风险
//   - backtrace() — 非严格 async-signal-safe; installCrashHandler 预热一次 dlopen,
//     运行期风险极低但无法完全消除(内部仍有 libgcc 的 unwind 锁)
//   - backtrace_symbols_fd() — glibc 实现不 malloc、直写 fd, 实践上安全, 但同样非 POSIX 保证
// 总体策略: 尽力打出调用栈, 接受极低概率的二次崩溃风险, 比什么都不记要好。
// 打完恢复默认 handler 重新 raise, 保留原本的退出码 / core dump 行为。
//
// altstack: installCrashHandler 只给调用线程装一块备用栈 (栈溢出型崩溃的保险)。
// 其余线程由 BxThread::run() 各自安装独立的 thread_local 备用栈。

namespace bronx {

// 装崩溃 handler。crashFile 为绝对路径(cwd 可能已被 anchor 改过),
// 崩溃栈追加写到该文件, 同时也写一份到 STDERR。多次调用只装一次。
// 返回 false 表示 altstack 分配失败(handler 仍会装, 只是栈溢出型崩溃可能打不全)。
bool installCrashHandler(const char* crashFile);

} // namespace bronx
