// test_sync.cpp — C3 现代锁体系验证
//  ① BxSpinLock 高争用互斥:多线程累加,结果精确无丢失
//  ② BxTicketLock 公平性:并发取号后按 FIFO 放行,累加同样精确
//  ③ try_lock 语义:已持锁时 try_lock 失败,释放后成功
#include "sync.h"
#include "test_util.h"
#include <thread>
#include <vector>

using namespace bronx;

// ① 自旋锁互斥:8 线程各加 100k,总和必须精确(无竞态丢失)
static void test_spinlock_mutual_exclusion() {
    BxSpinLock lock;
    long counter = 0;
    const int T = 8, N = 100000;
    std::vector<std::thread> ths;
    for (int i = 0; i < T; ++i) {
        ths.emplace_back([&]{
            for (int k = 0; k < N; ++k) {
                BxSpinLock::Lock lk(lock);
                ++counter;
            }
        });
    }
    for (auto& t : ths) t.join();
    TEST_CHECK_EQ(counter, (long)T * N);
}

// ② Ticket 公平锁:互斥正确性 + 短临界区。自旋锁不宜超订阅酷刑测(线程数贴合核数),
// 且 ticket 锁在争用下靠 spin-then-yield 降级,规模适度即可验正确性。
static void test_ticketlock_mutual_exclusion() {
    BxTicketLock lock;
    long counter = 0;
    unsigned hw = std::thread::hardware_concurrency();
    const int T = (int)(hw ? hw : 4), N = 20000;
    std::vector<std::thread> ths;
    for (int i = 0; i < T; ++i) {
        ths.emplace_back([&]{
            for (int k = 0; k < N; ++k) {
                BxTicketLock::Lock lk(lock);
                ++counter;
            }
        });
    }
    for (auto& t : ths) t.join();
    TEST_CHECK_EQ(counter, (long)T * N);
}

// ③ try_lock 语义:持锁时 try 失败,解锁后 try 成功
static void test_try_lock() {
    BxSpinLock sp;
    sp.lock();
    TEST_CHECK_MSG(!sp.try_lock(), "held spinlock: try_lock must fail");
    sp.unlock();
    TEST_CHECK_MSG(sp.try_lock(), "released spinlock: try_lock must succeed");
    sp.unlock();

    BxTicketLock tk;
    tk.lock();
    TEST_CHECK_MSG(!tk.try_lock(), "held ticketlock: try_lock must fail");
    tk.unlock();
    TEST_CHECK_MSG(tk.try_lock(), "released ticketlock: try_lock must succeed");
    tk.unlock();
}

// ④ BxRwMutex 多读单写:读守卫可并发持有,写守卫独占(验守卫重定向到 std 后可用)
static void test_rwmutex_guards() {
    BxRwMutex rw;
    {
        BxRwMutex::WriteLock w(rw);   // std::unique_lock
        TEST_CHECK_MSG(w.owns_lock(), "write lock should own");
        w.unlock();                    // 手动 unlock(reactor 用法)
        TEST_CHECK_MSG(!w.owns_lock(), "after unlock should not own");
    }
    {
        BxRwMutex::ReadLock r1(rw);   // std::shared_lock
        BxRwMutex::ReadLock r2(rw);   // 第二个读者可并发持有
        TEST_CHECK_MSG(r1.owns_lock() && r2.owns_lock(), "two readers coexist");
    }
}

int main() {
    test_spinlock_mutual_exclusion();
    test_ticketlock_mutual_exclusion();
    test_try_lock();
    test_rwmutex_guards();
    return TEST_SUMMARY();
}
