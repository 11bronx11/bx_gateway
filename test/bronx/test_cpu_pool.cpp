// test_cpu_pool.cpp — BxCpuPool + offload 回归测试
// 覆盖：basic submit / stats / queue full / drain / offload(int) /
//       offload(void) / exception传播 / inline降级 / 并发offload
#include "test_util.h"
#include "cpu_pool.h"
#include "offload.h"
#include "reactor.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <memory>
#include <system_error>
#include <vector>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();
static bronx::BxIoManager* g_cancel_iom = nullptr;

static void cancel_offload_wait_fd_for_test(int fd) {
    bronx::BxIoManager* iom = g_cancel_iom;
    if (!iom) {
        return;
    }
    iom->addTimer(10, [iom, fd]{
        iom->abortEvent(fd, bronx::BxIoManager::EV_IN);
    });
}

// ── T1: 基础 submit ─────────────────────────────────────────
static void test_pool_basic() {
    bronx::BxCpuPool pool(bronx::BxCpuPool::BxConfig{2});
    std::atomic<int> count{0};
    for (int i = 0; i < 8; ++i)
        pool.submit([&count]{ count.fetch_add(1, std::memory_order_relaxed); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TEST_CHECK_EQ(count.load(), 8);
}

// ── T2: stats ───────────────────────────────────────────────
static void test_pool_stats() {
    bronx::BxCpuPool pool(bronx::BxCpuPool::BxConfig{1, 0, "test"});
    std::atomic<bool> block{true};
    pool.submit([&block]{ while(block.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto s = pool.getStats();
    TEST_CHECK_EQ(s.submitted, 1u);
    TEST_CHECK_EQ(s.active, 1u);
    TEST_CHECK_EQ(s.rejected, 0u);
    block.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    s = pool.getStats();
    TEST_CHECK_EQ(s.completed, 1u);
    TEST_CHECK_EQ(s.active, 0u);
}

// ── T3: queue full ───────────────────────────────────────────
static void test_pool_queue_full() {
    // 1线程，队列上限2：1个运行 + 2个排队 = 第4个被拒
    bronx::BxCpuPool pool(bronx::BxCpuPool::BxConfig{1, 2, "qfull"});
    std::atomic<bool> block{true};
    pool.submit([&block]{ while(block.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 确保worker已拿走第1个
    pool.submit([]{});
    pool.submit([]{});
    bool ok = pool.submit([]{});  // 队列满（maxQueue=2已满）
    TEST_CHECK(!ok);
    auto s = pool.getStats();
    TEST_CHECK_EQ(s.rejected, 1u);
    block.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ── T4: drain ───────────────────────────────────────────────
static void test_pool_drain() {
    bronx::BxCpuPool pool(bronx::BxCpuPool::BxConfig{2});
    std::atomic<int> done{0};
    for (int i = 0; i < 6; ++i)
        pool.submit([&done]{
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            done.fetch_add(1, std::memory_order_relaxed);
        });
    pool.drain();  // 阻塞直到全部完成
    TEST_CHECK_EQ(done.load(), 6);
    // drain后拒绝新任务
    bool ok = pool.submit([]{});
    TEST_CHECK(!ok);
}

// ── T5: offload 返回 int ─────────────────────────────────────
static void test_offload_int() {
    auto gpool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{2});
    bronx::BxCpuPool::SetDefault(gpool);

    int result = 0;
    {
        bronx::BxIoManager iom(1, "offload-int");
        iom.post([&result]{
            result = bronx::offload([]{
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return 42;
            });
        });
    }
    TEST_CHECK_EQ(result, 42);
}

// ── T6: offload void ────────────────────────────────────────
static void test_offload_void() {
    auto gpool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{1});
    bronx::BxCpuPool::SetDefault(gpool);

    std::atomic<bool> ran{false};
    {
        bronx::BxIoManager iom(1, "offload-void");
        iom.post([&ran]{
            bronx::offload([]{
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            });
            ran.store(true);
        });
    }
    TEST_CHECK(ran.load());
}

// ── T7: exception 传播 ───────────────────────────────────────
static void test_offload_exception() {
    auto gpool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{1});
    bronx::BxCpuPool::SetDefault(gpool);

    bool caught = false;
    {
        bronx::BxIoManager iom(1, "offload-exc");
        iom.post([&caught]{
            try {
                bronx::offload([]{
                    throw std::runtime_error("cpu task error");
                    return 0;
                });
            } catch (const std::runtime_error& e) {
                caught = (std::string(e.what()) == "cpu task error");
            }
        });
    }
    TEST_CHECK(caught);
}

// ── T8: inline降级（非协程上下文）───────────────────────────
static void test_offload_inline() {
    auto gpool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{1});
    bronx::BxCpuPool::SetDefault(gpool);
    // 直接在主线程调用，无Fiber上下文 → inline执行
    int r = bronx::offload([]{ return 99; });
    TEST_CHECK_EQ(r, 99);
}

// ── T9: 并发offload ──────────────────────────────────────────
static void test_offload_concurrent() {
    auto gpool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{4});
    bronx::BxCpuPool::SetDefault(gpool);

    std::atomic<int> total{0};
    {
        bronx::BxIoManager iom(2, "offload-concurrent");
        for (int i = 0; i < 8; ++i) {
            iom.post([&total, i]{
                int r = bronx::offload([i]{ return i * i; });
                total.fetch_add(r, std::memory_order_relaxed);
            });
        }
    }
    // 0+1+4+9+16+25+36+49 = 140
    TEST_CHECK_EQ(total.load(), 140);
}

// ── T10: 边界：空任务拒绝 ─────────────────────────────────
static void test_pool_reject_empty_task() {
    bronx::BxCpuPool pool(bronx::BxCpuPool::BxConfig{1});
    bool ok = pool.submit(std::function<void()>());
    TEST_CHECK(!ok);
    auto s = pool.getStats();
    TEST_CHECK_EQ(s.submitted, 0u);
    TEST_CHECK_EQ(s.rejected, 1u);
}

// ── T11: 边界：worker 内 drain 不允许自等待 ───────────────
static void test_pool_drain_from_worker_rejected() {
    bronx::BxCpuPool pool(bronx::BxCpuPool::BxConfig{1});
    std::atomic<bool> caught{false};
    pool.submit([&]{
        try {
            pool.drain();
        } catch (const std::logic_error&) {
            caught.store(true, std::memory_order_relaxed);
        }
    });
    pool.drain();
    TEST_CHECK(caught.load(std::memory_order_relaxed));
}

// ── T12: 默认池切换时 offload 持有 shared_ptr 保活 ───────
static void test_offload_default_reset_race() {
    auto pool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{2});
    bronx::BxCpuPool::SetDefault(pool);
    std::atomic<int> entered{0};
    std::atomic<int> total{0};
    {
        bronx::BxIoManager iom(2, "offload-reset");
        for (int i = 0; i < 16; ++i) {
            iom.post([&]{
                entered.fetch_add(1, std::memory_order_relaxed);
                int r = bronx::offload([]{
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    return bronx::BxIoManager::Current() ? 0 : 1;
                });
                total.fetch_add(r, std::memory_order_relaxed);
            });
        }
        for (int i = 0; i < 1000 && entered.load(std::memory_order_relaxed) < 16; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_CHECK_EQ(entered.load(std::memory_order_relaxed), 16);
        bronx::BxCpuPool::SetDefault(nullptr);
        pool.reset();
    }
    TEST_CHECK_EQ(total.load(), 16);
}

// ── T13: 默认池切换时旧池析构不能持有默认池锁 ─────
static void test_set_default_reset_does_not_hold_default_lock_while_joining() {
    bronx::BxCpuPool::SetDefault(std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{1, 0, "default-reset"}));
    bronx::BxCpuPool* raw = bronx::BxCpuPool::GetDefault();
    TEST_CHECK(raw != nullptr);

    std::atomic<bool> task_entered{false};
    std::atomic<bool> release_task{false};
    std::atomic<bool> reset_entered{false};
    std::atomic<bool> reset_done{false};
    std::atomic<bool> worker_saw_null{false};

    bool ok = raw->submit([&]{
        task_entered.store(true, std::memory_order_release);
        while (!release_task.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        worker_saw_null.store(bronx::BxCpuPool::GetDefaultPtr() == nullptr, std::memory_order_release);
    });
    TEST_CHECK(ok);

    for (int i = 0; i < 1000 && !task_entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(task_entered.load(std::memory_order_acquire));

    std::thread resetter([&]{
        reset_entered.store(true, std::memory_order_release);
        bronx::BxCpuPool::SetDefault(nullptr);
        reset_done.store(true, std::memory_order_release);
    });

    for (int i = 0; i < 1000 && !reset_entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(reset_entered.load(std::memory_order_acquire));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    release_task.store(true, std::memory_order_release);
    resetter.join();

    TEST_CHECK(reset_done.load(std::memory_order_acquire));
    TEST_CHECK(worker_saw_null.load(std::memory_order_acquire));
}

// ── T14: worker 内清空默认池不能 self-join ─────────────
static void test_set_default_from_worker_does_not_self_join() {
    auto pool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{1, 0, "self-reset"});
    bronx::BxCpuPool::SetDefault(pool);
    bronx::BxCpuPool* raw = pool.get();

    std::atomic<bool> release_task{false};
    std::atomic<bool> done{false};
    bool ok = raw->submit([&]{
        while (!release_task.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bronx::BxCpuPool::SetDefault(nullptr);
        done.store(true, std::memory_order_release);
    });
    TEST_CHECK(ok);

    pool.reset();
    release_task.store(true, std::memory_order_release);
    for (int i = 0; i < 1000 && !done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(done.load(std::memory_order_acquire));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

// ── T15: submit(timeout) 背压等待队列空间 ──────────────────
static void test_pool_submit_timeout_waits_for_space() {
    bronx::BxCpuPool pool(bronx::BxCpuPool::BxConfig{1, 1, "timeout-ok"});
    std::atomic<bool> release{false};
    std::atomic<bool> queued_ran{false};
    TEST_CHECK(pool.trySubmit([&]{
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_CHECK(pool.trySubmit([&]{ queued_ran.store(true, std::memory_order_release); }));

    std::thread releaser([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        release.store(true, std::memory_order_release);
    });
    bool ok = pool.submit([]{}, std::chrono::milliseconds(200));
    releaser.join();
    TEST_CHECK(ok);
    pool.drain();
    TEST_CHECK(queued_ran.load(std::memory_order_acquire));
}

// ── T16: submit(timeout) 超时拒绝 ───────────────────────────
static void test_pool_submit_timeout_rejects_when_full() {
    bronx::BxCpuPool pool(bronx::BxCpuPool::BxConfig{1, 1, "timeout-reject"});
    std::atomic<bool> release{false};
    TEST_CHECK(pool.trySubmit([&]{
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_CHECK(pool.trySubmit([]{}));
    bool ok = pool.submit([]{}, std::chrono::milliseconds(10));
    TEST_CHECK(!ok);
    auto s = pool.getStats();
    TEST_CHECK_EQ(s.rejected, 1u);
    release.store(true, std::memory_order_release);
    pool.drain();
}

// ── T17: BxCpuPool 支持 move-only callable ────────────────────
static void test_pool_move_only_callable() {
    bronx::BxCpuPool pool(bronx::BxCpuPool::BxConfig{1, 0, "move-callable"});
    std::atomic<int> value{0};
    auto ptr = std::make_unique<int>(7);
    bool ok = pool.trySubmit([p = std::move(ptr), &value]() mutable {
        value.store(*p, std::memory_order_release);
    });
    TEST_CHECK(ok);
    pool.drain();
    TEST_CHECK_EQ(value.load(std::memory_order_acquire), 7);
}

// ── T18: offload 支持 move-only callable / return ───────────
static void test_offload_move_only_types() {
    auto gpool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{2, 0, "move-offload"});
    bronx::BxCpuPool::SetDefault(gpool);

    int value = 0;
    {
        bronx::BxIoManager iom(1, "offload-move");
        iom.post([&]{
            auto captured = std::make_unique<int>(17);
            auto result = bronx::offload([p = std::move(captured)]() mutable {
                return std::make_unique<int>(*p + 1);
            });
            value = *result;
        });
    }
    TEST_CHECK_EQ(value, 18);
}

struct NoDefault {
    explicit NoDefault(int v) : value(v) {}
    NoDefault(const NoDefault&) = delete;
    NoDefault& operator=(const NoDefault&) = delete;
    NoDefault(NoDefault&&) noexcept = default;
    NoDefault& operator=(NoDefault&&) noexcept = default;
    int value;
};

// ── T19: offload 支持不可默认构造返回值 ────────────────────
static void test_offload_non_default_return() {
    auto gpool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{1, 0, "nodefault"});
    bronx::BxCpuPool::SetDefault(gpool);

    int value = 0;
    {
        bronx::BxIoManager iom(1, "offload-nodefault");
        iom.post([&]{
            NoDefault r = bronx::offload([]{ return NoDefault(23); });
            value = r.value;
        });
    }
    TEST_CHECK_EQ(value, 23);
}

// ── T20: CPU任务先完成也不重复调度RUNNING fiber ─────────────
static void test_offload_fast_completion_race() {
    auto gpool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{4, 0, "fast-race"});
    bronx::BxCpuPool::SetDefault(gpool);

    std::atomic<int> total{0};
    {
        bronx::BxIoManager iom(2, "offload-fast");
        for (int i = 0; i < 200; ++i) {
            iom.post([&]{
                int r = bronx::offload([]{ return 1; });
                total.fetch_add(r, std::memory_order_relaxed);
            });
        }
    }
    TEST_CHECK_EQ(total.load(std::memory_order_relaxed), 200);
}

// ── T21: offload等待被cancel时返回ECANCELED且后台任务仍安全完成 ─
static void test_offload_wait_cancel_safe() {
    auto gpool = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{1, 0, "cancel-safe"});
    bronx::BxCpuPool::SetDefault(gpool);

    std::atomic<bool> task_entered{false};
    std::atomic<bool> release_task{false};
    std::atomic<bool> cpu_completed{false};
    std::atomic<bool> caught_cancel{false};
    std::atomic<bool> fiber_done{false};

    {
        bronx::BxIoManager iom(1, "offload-cancel");
        g_cancel_iom = &iom;
        bronx::detail::SetOffloadWaitObserverForTest(cancel_offload_wait_fd_for_test);
        iom.post([&]{
            try {
                bronx::offload([&]{
                    task_entered.store(true, std::memory_order_release);
                    while (!release_task.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    cpu_completed.store(true, std::memory_order_release);
                    return 1;
                });
            } catch (const std::system_error& e) {
                caught_cancel.store(e.code().value() == ECANCELED, std::memory_order_release);
            }
            fiber_done.store(true, std::memory_order_release);
        });
        while (!caught_cancel.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        release_task.store(true, std::memory_order_release);
        while (!cpu_completed.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bronx::detail::SetOffloadWaitObserverForTest(nullptr);
        g_cancel_iom = nullptr;
    }

    TEST_CHECK(fiber_done.load(std::memory_order_acquire));
    TEST_CHECK(caught_cancel.load(std::memory_order_acquire));
    TEST_CHECK(cpu_completed.load(std::memory_order_acquire));
}

// ── T10: SetDefault/GetDefault ──────────────────────────────
static void test_default_instance() {
    bronx::BxCpuPool::SetDefault(nullptr);
    TEST_CHECK(bronx::BxCpuPool::GetDefault() == nullptr);  // nullptr后inline降级

    auto p = std::make_shared<bronx::BxCpuPool>(bronx::BxCpuPool::BxConfig{1});
    bronx::BxCpuPool::SetDefault(p);
    TEST_CHECK(bronx::BxCpuPool::GetDefault() != nullptr);
    TEST_CHECK(bronx::BxCpuPool::GetDefault() == p.get());
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_cpu_pool start ===";
    test_pool_basic();
    test_pool_stats();
    test_pool_queue_full();
    test_pool_drain();
    test_offload_int();
    test_offload_void();
    test_offload_exception();
    test_offload_inline();
    test_offload_concurrent();
    test_pool_reject_empty_task();
    test_pool_drain_from_worker_rejected();
    test_offload_default_reset_race();
    test_set_default_reset_does_not_hold_default_lock_while_joining();
    test_set_default_from_worker_does_not_self_join();
    test_pool_submit_timeout_waits_for_space();
    test_pool_submit_timeout_rejects_when_full();
    test_pool_move_only_callable();
    test_offload_move_only_types();
    test_offload_non_default_return();
    test_offload_fast_completion_race();
    test_offload_wait_cancel_safe();
    test_default_instance();
    return TEST_SUMMARY();
}
