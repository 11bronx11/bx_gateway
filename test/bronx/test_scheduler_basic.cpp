// 阶段0 基线 + 阶段1 回归:BxScheduler / BxIoManager 基础调度正确性
// =================================================================
// 目的:
//   1. 验证测试地基(test_util.h 断言宏)本身可用。
//   2. 为"去 use_caller 简化"建立基线 —— 改动前后这些行为必须不变:
//      - 大量任务全部执行且仅执行一次(无丢、无重复)
//      - 多线程并发调度计数正确
//      - 协程内 yield/resume 往返正确
//      - BxIoManager 定时器按时触发
//   竞态/cancel 的专门测试在后续阶段单独文件。

#include "test_util.h"
#include "reactor.h"
#include "exec.h"
#include "fiber.h"
#include <atomic>
#include <unistd.h>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

// 1. 大量任务恰好执行一次(无丢失/无重复)
static void test_schedule_count() {
    const int N = 2000;
    std::atomic<int> counter{0};
    {
        // 2 线程工作池
        bronx::BxIoManager iom(2);
        for(int i = 0; i < N; ++i) {
            iom.post([&counter](){ counter.fetch_add(1); });
        }
        // iom 析构 = stop = 等所有任务跑完
    }
    TEST_CHECK_EQ(counter.load(), N);
}

// 2. 协程内 sleep(hook) 不阻塞线程,且能正常恢复
static void test_fiber_sleep_resume() {
    std::atomic<int> done{0};
    uint64_t start = bronx_test::now_ms();
    {
        bronx::BxIoManager iom(2);
        for(int i = 0; i < 4; ++i) {
            iom.post([&done](){
                sleep(1);            // hook 版:协程让出,不真阻塞线程
                done.fetch_add(1);
            });
        }
    }
    uint64_t elapsed = bronx_test::now_ms() - start;
    TEST_CHECK_EQ(done.load(), 4);
    // 4 个 sleep(1) 并发,应在 ~1s 左右完成,远小于串行的 4s
    TEST_CHECK_MSG(elapsed < 3000, "4x concurrent sleep(1) took " << elapsed << "ms (expected ~1s)");
}

// 3. 定时器按时触发 + 循环定时器
static void test_timer() {
    std::atomic<int> tick{0};
    {
        bronx::BxIoManager iom(1);
        iom.addTimer(100, [&tick](){ tick.fetch_add(1); }, false);   // 单次
        bronx::BxTimer::ptr t = iom.addTimer(50, [&tick](){ tick.fetch_add(1); }, true); // 循环
        // 让循环定时器跑几次后取消
        iom.addTimer(280, [t](){ t->cancel(); }, false);
        iom.addTimer(500, [](){}, false); // 兜底:撑住 iom 不要太早 stop
    }
    // 单次1 + 循环在 280ms 内约触发 5 次(50/100/150/200/250)。放宽断言只验证"触发过且非失控"
    TEST_CHECK_MSG(tick.load() >= 3, "timer fired " << tick.load() << " times (expected >=3)");
    TEST_CHECK_MSG(tick.load() <= 12, "timer fired " << tick.load() << " times (runaway?)");
}

// 4. 协程 yield/resume 往返:子协程多次让出再恢复,顺序正确
static void test_fiber_roundtrip() {
    std::atomic<int> seq{0};
    bool order_ok = true;
    {
        bronx::BxIoManager iom(1);
        iom.post([&seq, &order_ok](){
            // 在协程里通过 sleep(0 级) 触发若干次调度往返
            for(int i = 0; i < 5; ++i) {
                int before = seq.fetch_add(1);
                if(before != i) order_ok = false;
                usleep(1000); // hook:让出再回来
            }
        });
    }
    TEST_CHECK_EQ(seq.load(), 5);
    TEST_CHECK_MSG(order_ok, "fiber resume order broken");
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_scheduler_basic start ===";
    test_schedule_count();
    test_fiber_sleep_resume();
    test_timer();
    test_fiber_roundtrip();
    return TEST_SUMMARY();
}
