// test_stack_pool.cpp — C1 协程栈池 + guard page 验证
//  ① 池复用:同尺寸反复 Alloc/Dealloc,mmap 只发生一次,其余全走池复用
//  ② guard page:写保护页(可用区下方一页)立即 SIGSEGV —— fork 子进程验证
#include "stack_pool.h"
#include "test_util.h"
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>
#include <cstring>

using namespace bronx;

// ① 池复用计数:cap 内反复 Alloc→Dealloc,应只 mmap 一次,复用 N-1 次
static void test_pool_reuse() {
    BxStackAllocator::Configure(4, true);  // 首次配置生效
    const size_t sz = 128 * 1024;
    const int N = 50;

    // 预热一次:确保该尺寸首块已产生(避免把别处的 mmap 算进来)
    void* warm = BxStackAllocator::Alloc(sz);
    BxStackAllocator::Dealloc(warm, sz);

    auto before = BxStackAllocator::Stats();
    for (int i = 0; i < N; ++i) {
        void* p = BxStackAllocator::Alloc(sz);
        TEST_CHECK_MSG(p != nullptr, "alloc should succeed");
        BxStackAllocator::Dealloc(p, sz);
    }
    auto after = BxStackAllocator::Stats();

    uint64_t mmapDelta  = after.mmaps  - before.mmaps;
    uint64_t reuseDelta = after.reuses - before.reuses;
    // 预热后池里已有 1 块,N 次循环应全部命中池,零新 mmap
    TEST_CHECK_MSG(mmapDelta == 0, "pooled reuse must not mmap again");
    TEST_CHECK_EQ(reuseDelta, (uint64_t)N);
}

// ② guard page death test:子进程写可用区下方的保护页,应被 SIGSEGV 杀
static void test_guard_page() {
#if defined(__SANITIZE_ADDRESS__)
    // ASan 会抢先拦截 SEGV(DEADLYSIGNAL→abort),子进程非纯 SIGSEGV 退出,
    // death test 语义在 ASan 下不成立;越界访问本身已由 ASan 覆盖,这里跳过。
    BRONX_LOG_INFO(bronx_test::logger()) << "guard page death test skipped under ASan";
    return;
#endif
    BxStackAllocator::Configure(4, true);
    const size_t sz = 128 * 1024;
    long ps = sysconf(_SC_PAGESIZE);

    pid_t pid = fork();
    TEST_CHECK_MSG(pid >= 0, "fork should succeed");
    if (pid == 0) {
        // 子进程:拿一块栈,往可用区起址下方(guard page 内)写 → 触发 SIGSEGV
        void* usable = BxStackAllocator::Alloc(sz);
        if (!usable) _exit(42);              // 分配失败,用特殊码区分
        volatile char* guard = (char*)usable - ps / 2;  // 落在 guard page 里
        *guard = 'x';                        // 应在此崩溃
        _exit(0);                            // 没崩=guard 失效
    }

    int status = 0;
    waitpid(pid, &status, 0);
    bool segv = WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV);
    TEST_CHECK_MSG(segv, "writing guard page must raise SIGSEGV");
}

int main() {
    test_pool_reuse();
    test_guard_page();
    return TEST_SUMMARY();
}
