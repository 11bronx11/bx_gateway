#include "pool.h"
#include "mempool.h"
#include "buf.h"
#include "test_util.h"
#include <cstring>
#include <thread>
#include <vector>

using namespace bronx::gateway;

// T1: 5 个 class 各 alloc/free 一次
static void test_basic() {
    GwPool& p = GwPool::get();
    for(int cls = 0; cls < kNcls; ++cls) {
        void* ptr = p.alloc(cls);
        TEST_CHECK(ptr != nullptr);
        // 写一下用户区，确认没越界崩溃
        memset(ptr, 0xAB, kClsSizes[cls]);
        p.free(cls, ptr);
    }
}

// T2: 反复 alloc/free，TLS 命中为主
static void test_tls_hit() {
    GwPool& p = GwPool::get();
    auto s0 = p.stats();

    for(int i = 0; i < 500; ++i) {
        void* ptr = p.alloc(0);  // 4K
        TEST_CHECK(ptr != nullptr);
        p.free(0, ptr);
    }

    auto s1 = p.stats();
    TEST_CHECK(s1.tls_hits > s0.tls_hits);
}

// T3: 强制触发 drain→fill→mmap（把 TLS 高水位打满）
static void test_overflow() {
    GwPool& p = GwPool::get();
    auto s0 = p.stats();

    // 分配 300 个，超过默认高水位 16，触发多次 fill
    std::vector<void*> ptrs;
    ptrs.reserve(300);
    for(int i = 0; i < 300; ++i)
        ptrs.push_back(p.alloc(0));
    for(auto ptr : ptrs)
        p.free(0, ptr);

    auto s1 = p.stats();
    TEST_CHECK(s1.mmaps > s0.mmaps);
    TEST_CHECK(s1.refills > s0.refills);
}

// T4: 非标大小走 fallback，不入池
static void test_fallback() {
    // sizeClass 返回 -1 的大小
    TEST_CHECK_EQ(sizeClass(3000),  -1);
    TEST_CHECK_EQ(sizeClass(70000), -1);
    TEST_CHECK_EQ(sizeClass(4096),   0);
    TEST_CHECK_EQ(sizeClass(65536),  4);

    // 通过 MemAlloc 走 fallback 路径
    void* p1 = MemAlloc::alloc(3000);
    TEST_CHECK(p1 != nullptr);
    MemAlloc::free(p1, 3000);

    void* p2 = MemAlloc::alloc(70000);
    TEST_CHECK(p2 != nullptr);
    MemAlloc::free(p2, 70000);
    MemAlloc::free(nullptr, 4096);
}

// T4b: InBuf 的标准容量分配要经过 GwPool
static void test_inbuf_integration() {
    GwPool& p = GwPool::get();
    auto s0 = p.stats();
    {
        InBuf b(4096);
        std::string data(4097, 'x');
        b.append(data.data(), data.size());
    }
    auto s1 = p.stats();
    uint64_t before = s0.tls_hits + s0.central_hits + s0.refills;
    uint64_t after = s1.tls_hits + s1.central_hits + s1.refills;
    TEST_CHECK(after > before);
}

// T5: 多线程并发 alloc/free（4 线程各 1000 次）
static void test_cross_thread() {
    GwPool& p = GwPool::get();
    std::atomic<int> errs{0};

    auto worker = [&]() {
        for(int i = 0; i < 1000; ++i) {
            int cls = i % kNcls;
            void* ptr = p.alloc(cls);
            if(!ptr) { ++errs; continue; }
            memset(ptr, 0, kClsSizes[cls]);
            p.free(cls, ptr);
        }
        // 线程退出前把 TLS bins 还给中心层，否则 Span 对象泄漏
        p.drainLocal();
    };

    std::vector<std::thread> ts;
    for(int i = 0; i < 4; ++i) ts.emplace_back(worker);
    for(auto& t : ts) t.join();

    TEST_CHECK_EQ(errs.load(), 0);
}

// T6: 把一个 Span 的所有块全部 free，触发 munmap
static void test_munmap() {
    GwPool& p = GwPool::get();
    auto s0 = p.stats();

    // cls=4 (64K), kSpanBlocks[4]=2，分配 2 个块就能占满一个 Span
    // 多分配几批确保至少有一个 Span 被填满再全 free
    const int cls = 4;
    const int n   = kSpanBlocks[cls] * 3;  // 跨多个 Span
    std::vector<void*> ptrs(n);
    for(int i = 0; i < n; ++i) ptrs[i] = p.alloc(cls);
    for(int i = 0; i < n; ++i) p.free(cls, ptrs[i]);

    // drain 会把块还给中心层，中心层块全部回来后触发 munmap
    // 强制 drain：再分配 256 个触发高水位归还
    std::vector<void*> extra(256);
    for(int i = 0; i < 256; ++i) extra[i] = p.alloc(0);
    for(int i = 0; i < 256; ++i) p.free(0, extra[i]);

    auto s1 = p.stats();
    TEST_CHECK(s1.munmaps > s0.munmaps);
}

int main() {
    test_basic();
    test_tls_hit();
    test_overflow();
    test_fallback();
    test_inbuf_integration();
    test_cross_thread();
    test_munmap();
    return TEST_SUMMARY();
}
