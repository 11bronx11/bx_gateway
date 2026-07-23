// 定时器正确性回归测试
// =====================
// 守护 BUG-B(timer.h: m_timers 未指定 BxTimer::Comparator → 按指针地址而非 m_next 排序):
//   - 多个定时器必须按超时时间(而非添加/地址顺序)触发
//   - 循环定时器在窗口内按周期反复触发
//   - 单次定时器只触发一次

#include "test_util.h"
#include "reactor.h"
#include "timer.h"
#include <vector>
#include <unistd.h>
#include <thread>
#include <atomic>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

// 1. 多定时器按时间顺序触发(乱序添加,必须按 m_next 升序触发)
static void test_timer_order() {
    std::vector<int> order;
    {
        bronx::BxIoManager iom(1);
        // 故意乱序添加:期望触发顺序应为 1,2,3,4(按延时)
        iom.addTimer(200, [&order](){ order.push_back(4); }, false);
        iom.addTimer(50,  [&order](){ order.push_back(1); }, false);
        iom.addTimer(150, [&order](){ order.push_back(3); }, false);
        iom.addTimer(100, [&order](){ order.push_back(2); }, false);
        usleep(350 * 1000);
    }
    TEST_CHECK_EQ((int)order.size(), 4);
    if(order.size() == 4) {
        bool sorted = (order[0]==1 && order[1]==2 && order[2]==3 && order[3]==4);
        TEST_CHECK_MSG(sorted, "timer fire order wrong: "
            << order[0] << order[1] << order[2] << order[3] << " (expected 1234)");
    }
}

// 2. 循环定时器在窗口内按周期触发
static void test_recurring() {
    std::vector<uint64_t> fires;
    uint64_t t0 = bronx_test::now_ms();
    {
        bronx::BxIoManager iom(1);
        bronx::BxTimer::ptr t = iom.addTimer(50, [&fires, t0](){
            fires.push_back(bronx_test::now_ms() - t0);
        }, true);
        usleep(320 * 1000);
        t->cancel();
    }
    // 320ms / 50ms ≈ 5-6 次
    TEST_CHECK_MSG(fires.size() >= 4 && fires.size() <= 8,
        "recurring 50ms over 320ms fired " << fires.size() << " (expected ~6)");
    // 时间戳应单调递增
    bool monotonic = true;
    for(size_t i = 1; i < fires.size(); ++i) {
        if(fires[i] <= fires[i-1]) monotonic = false;
    }
    TEST_CHECK_MSG(monotonic, "recurring fire timestamps not monotonic");
}

// 3. 单次定时器只触发一次
static void test_oneshot_once() {
    std::atomic<int> n{0};
    {
        bronx::BxIoManager iom(1);
        iom.addTimer(50, [&n](){ n.fetch_add(1); }, false);
        usleep(250 * 1000);
    }
    TEST_CHECK_EQ(n.load(), 1);
}

// 4. 混合场景(复刻当初暴露 bug 的用例):单次100 + 循环50 + 取消 + 兜底
static void test_mixed() {
    std::vector<uint64_t> fires;
    uint64_t t0 = bronx_test::now_ms();
    {
        bronx::BxIoManager iom(1);
        iom.addTimer(100, [&fires,t0](){ fires.push_back(bronx_test::now_ms()-t0); }, false);
        bronx::BxTimer::ptr t = iom.addTimer(50, [&fires,t0](){ fires.push_back(bronx_test::now_ms()-t0); }, true);
        iom.addTimer(280, [t](){ t->cancel(); }, false);
        iom.addTimer(500, [](){}, false);
        usleep(600 * 1000);
    }
    // 100单次(1) + 50循环在280ms内约5次 = ~6
    TEST_CHECK_MSG(fires.size() >= 5, "mixed scenario fired " << fires.size() << " (expected ~6, was 2 with bug)");
}

// 5. 优雅停止:挂着永不取消的循环定时器,stop()(析构)必须能正常返回,不挂死
static void test_stop_with_recurring() {
    std::atomic<int> ticks{0};
    uint64_t t0 = bronx_test::now_ms();
    {
        bronx::BxIoManager iom(1);
        // 永不取消的循环定时器(模拟心跳/清理),不手动 cancel
        iom.addTimer(30, [&ticks](){ ticks.fetch_add(1); }, true);
        usleep(150 * 1000);
        // 出作用域 → dtor → stop():方案1 下应忽略循环定时器,正常退出
    }
    uint64_t elapsed = bronx_test::now_ms() - t0;
    // 循环定时器期间触发过几次
    TEST_CHECK_MSG(ticks.load() >= 2, "recurring fired " << ticks.load() << " times");
    // 关键:stop 没挂死(总耗时接近 usleep 的 150ms,远小于任何超时兜底)
    TEST_CHECK_MSG(elapsed < 1000, "stop with recurring timer took " << elapsed << "ms (HANG?)");
}

// 6. reset 到更早时间时必须唤醒 epoll_wait,不能等旧的超时时间
static void test_reset_to_front() {
    std::atomic<bool> fired{false};
    uint64_t t0 = bronx_test::now_ms();
    uint64_t elapsed = 0;
    {
        bronx::BxIoManager iom(1);
        bronx::BxTimer::ptr t = iom.addTimer(500, [&](){
            elapsed = bronx_test::now_ms() - t0;
            fired = true;
        }, false);
        usleep(50 * 1000);
        TEST_CHECK(t->reset(30, true));
        usleep(180 * 1000);
    }
    TEST_CHECK_MSG(fired.load(), "reset timer did not fire");
    TEST_CHECK_MSG(elapsed >= 50 && elapsed < 250,
                   "reset timer fired at " << elapsed << "ms (expected around 80ms)");
}

// 7. reset/cancel 与调度线程并发时不能破坏 BxTimerManager 的 set 状态
static void test_reset_cancel_concurrent() {
    std::atomic<bool> stop{false};
    std::atomic<int> fires{0};
    {
        bronx::BxIoManager iom(2);
        bronx::BxTimer::ptr t = iom.addTimer(10, [&](){
            fires.fetch_add(1);
        }, true);
        std::thread worker([&](){
            for(int i = 0; i < 200; ++i){
                t->reset((i % 5) + 1, true);
                usleep(1000);
            }
            stop = true;
        });
        while(!stop.load()){
            usleep(1000);
        }
        worker.join();
        TEST_CHECK(t->cancel());
        usleep(20 * 1000);
    }
    TEST_CHECK_MSG(fires.load() > 0, "concurrent reset timer never fired");
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_timer start ===";
    test_timer_order();
    test_recurring();
    test_oneshot_once();
    test_mixed();
    test_stop_with_recurring();
    test_reset_to_front();
    test_reset_cancel_concurrent();
    return TEST_SUMMARY();
}
