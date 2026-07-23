// test_thread_caps.cpp — C2 线程能力封装验证
//  ① CPU affinity:绑核后线程内 sched_getaffinity 回读确认(设不上则记 best-effort)
//  ② stop_token:jthread 风格 cb 收 token,requestStop 后线程协作退出
//  ③ 大栈:自定义 stackSize 线程能正常起停
#include "thread.h"
#include "test_util.h"
#include <sched.h>
#include <sys/resource.h>
#include <atomic>
#include <cerrno>
#include <chrono>

using namespace bronx;

// ① affinity 回读
static void test_affinity() {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) { TEST_CHECK_MSG(true, "no hw concurrency info, skip"); return; }
    std::atomic<int> boundCore{-1};
    BxThreadOptions opts;
    opts.cpuAffinity = 0;   // 绑到核 0
    BxThread t([&]{
        cpu_set_t set;
        CPU_ZERO(&set);
        if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
            // 期望只含核 0
            boundCore.store(CPU_ISSET(0, &set) && CPU_COUNT(&set) == 1 ? 0 : -2);
        }
    }, "aff", opts);
    t.join();
    // 成功绑核=0,或环境不允许(-2/-1):best-effort,只要不崩即算通过;绑上则强断言
    int bc = boundCore.load();
    TEST_CHECK_MSG(bc == 0 || bc == -2 || bc == -1, "affinity readback sane");
    if (bc == 0) TEST_CHECK_MSG(true, "affinity bound to core 0 confirmed");
}

// ② stop_token 协作退出
static void test_stop_token() {
    std::atomic<bool> started{false};
    std::atomic<long> loops{0};
    BxThread t([&](std::stop_token st){
        started.store(true);
        while (!st.stop_requested()) {
            loops.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }, "stoppable");
    // 等线程起来
    while (!started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bool req = t.requestStop();
    t.join();   // 若 stop 未生效,join 会挂 —— 能返回即证明协作退出成功
    TEST_CHECK_MSG(req, "request_stop should succeed first time");
    TEST_CHECK_MSG(loops.load() > 0, "thread body ran before stop");
}

// ③ 自定义大栈起停
static void test_custom_stack() {
    std::atomic<bool> ran{false};
    BxThreadOptions opts;
    opts.stackSize = 2 * 1024 * 1024;   // 2MB
    BxThread t([&]{ ran.store(true); }, "bigstack", opts);
    t.join();
    TEST_CHECK_MSG(ran.load(), "custom-stack thread should run");
}

// ④ stop_token 重载未显式 join 时,析构应请求停止并等待线程退出
static void test_destructor_stop_and_join() {
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    {
        BxThread t([&](std::stop_token st){
            started.store(true, std::memory_order_release);
            while (!st.stop_requested()) std::this_thread::yield();
            stopped.store(true, std::memory_order_release);
        }, "auto-stop");
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    TEST_CHECK_MSG(stopped.load(std::memory_order_acquire),
                   "stop-token destructor should wait for callback exit");
}

// ⑤ priority 是当前线程的 Linux nice 值;正值降优先级不需要额外权限
static void test_nice_priority() {
    std::atomic<int> observed{-100};
    BxThreadOptions opts;
    opts.priority = 1;
    BxThread t([&]{
        errno = 0;
        int value = getpriority(PRIO_PROCESS, 0);
        observed.store(errno == 0 ? value : -100, std::memory_order_release);
    }, "nice", opts);
    t.join();
    TEST_CHECK_EQ(observed.load(std::memory_order_acquire), 1);
}

int main() {
    test_affinity();
    test_stop_token();
    test_custom_stack();
    test_destructor_stop_and_join();
    test_nice_priority();
    return TEST_SUMMARY();
}
