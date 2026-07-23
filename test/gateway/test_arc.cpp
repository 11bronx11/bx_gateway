// test_arc.cpp — Arc<K,V> 单元测试
// 覆盖: put/get 基本语义, 容量驱逐, T1→T2 升级, ghost 自适应, stats, invalidate, 并发安全

#include "arc.h"
#include "test_util.h"
#include <thread>
#include <vector>
#include <atomic>
#include <string>
#include <iostream>

using bronx::Arc;

// 1. 基本 put/get，命中值对
static void test_basic() {
    Arc<int,int> arc(8);
    arc.put(1, 100);
    arc.put(2, 200);
    int v = 0;
    TEST_CHECK(arc.get(1, v)); TEST_CHECK_EQ(v, 100);
    TEST_CHECK(arc.get(2, v)); TEST_CHECK_EQ(v, 200);
    TEST_CHECK(!arc.get(99, v));
    std::cout << "PASS: basic\n";
}

// 2. 容量驱逐：放 cap+1 个，最旧 miss
static void test_evict() {
    // 每个 shard 容量=1（总 cap=16，N=16），所以 cap/N=1
    Arc<int,int,16> arc(16);
    // 填满一个 shard（连续同 hash 的 key 才保证同一 shard，用倍数=16 的 key）
    // 更直接：cap 设成16，往同一 shard 塞 >1 个 key（用步长 16 的 key 进同一 shard）
    // 这里测总容量：放 17 个不同 key，第一个应该被驱逐（ARC 最终行为 key=0 miss）
    Arc<int,int,1> arc2(4);  // 单 shard，cap=4
    for(int i = 0; i < 4; ++i) arc2.put(i, i*10);
    arc2.put(4, 40);         // 触发驱逐
    int v = 0;
    // 最旧插入的 key=0 应该被驱出（T1 LRU）
    TEST_CHECK(!arc2.get(0, v));
    // 后面放的仍在
    TEST_CHECK(arc2.get(4, v)); TEST_CHECK_EQ(v, 40);
    std::cout << "PASS: evict\n";
}

// 2b. 总容量不能因固定分片被放大
static void test_exact_capacity() {
    Arc<int,int,16> arc(1);
    for(int i = 0; i < 16; ++i) arc.put(i, i);

    int v = 0;
    for(int i = 0; i < 15; ++i) TEST_CHECK(!arc.get(i, v));
    TEST_CHECK(arc.get(15, v));
    TEST_CHECK_EQ(v, 15);
    std::cout << "PASS: exact_capacity\n";
}

// 2c. 非 2 的幂分片数也要均匀路由
static void test_non_power_of_two_shards() {
    Arc<int,int,3> arc(3);
    for(int i = 0; i < 3; ++i) arc.put(i, i * 10);

    int v = 0;
    for(int i = 0; i < 3; ++i) {
        TEST_CHECK(arc.get(i, v));
        TEST_CHECK_EQ(v, i * 10);
    }
    std::cout << "PASS: non_power_of_two_shards\n";
}

// 3. T1→T2 升级：同 key 访问两次后，驱逐其他项时 T2 里的不被优先踢
static void test_t1_to_t2() {
    Arc<int,int,1> arc(4);   // 单 shard，cap=4
    for(int i = 0; i < 4; ++i) arc.put(i, i*10);
    // 访问 key=0 两次，升入 T2
    int v = 0;
    arc.get(0, v); // 第一次 get：T1→T2
    // 现在 key=0 在 T2，放新 key 触发驱逐，T1 里的应该先被踢
    arc.put(4, 40);
    // key=0 在 T2，不应被优先驱逐
    TEST_CHECK(arc.get(0, v)); TEST_CHECK_EQ(v, 0);
    std::cout << "PASS: t1_to_t2\n";
}

// 4. Ghost 自适应：B1 命中让 p 增大，后续 T1 更耐驱逐
static void test_ghost_adapt() {
    // 构造 B1 ghost 命中场景：放满→驱逐到 B1→重新 put 同 key 触发 B1 命中
    Arc<int,int,1> arc(2);   // 单 shard，cap=2
    arc.put(1, 10);
    arc.put(2, 20);
    arc.put(3, 30);  // 驱逐 key=1，key=1 进入 B1
    // 现在重新 put(1, ...) 触发 B1 命中，p 应该增大
    arc.put(1, 11);  // B1 命中 → key=1 进 T2，p++
    int v = 0;
    TEST_CHECK(arc.get(1, v)); TEST_CHECK_EQ(v, 11);
    std::cout << "PASS: ghost_adapt\n";
}

// 5. stats hit/miss 计数准确
static void test_stats() {
    Arc<std::string,int> arc(32);
    arc.put("a", 1); arc.put("b", 2); arc.put("c", 3);
    int v = 0;
    arc.get("a", v); arc.get("b", v); arc.get("missing", v);
    uint64_t hits = 0, misses = 0;
    arc.stats(hits, misses);
    TEST_CHECK_EQ(hits, (uint64_t)2);
    TEST_CHECK_EQ(misses, (uint64_t)1);
    std::cout << "PASS: stats\n";
}

// 6. invalidate 后全 miss
static void test_invalidate() {
    Arc<int,int> arc(32);
    for(int i = 0; i < 10; ++i) arc.put(i, i);
    arc.invalidate();
    int v = 0;
    for(int i = 0; i < 10; ++i) TEST_CHECK(!arc.get(i, v));
    std::cout << "PASS: invalidate\n";
}

// 7. 并发：8 线程混合 put/get 不崩，stats 合理
static void test_concurrent() {
    constexpr int THREADS = 8;
    constexpr int OPS     = 2000;
    Arc<int,int> arc(64);

    std::atomic<uint64_t> total_ops{0};
    std::vector<std::thread> ts;
    ts.reserve(THREADS);
    for(int t = 0; t < THREADS; ++t) {
        ts.emplace_back([&, t]() {
            int v = 0;
            for(int i = 0; i < OPS; ++i) {
                int k = (t * OPS + i) % 128;
                arc.put(k, k * 2);
                arc.get(k, v);
                total_ops.fetch_add(2, std::memory_order_relaxed);
            }
        });
    }
    for(auto& th : ts) th.join();

    uint64_t hits = 0, misses = 0;
    arc.stats(hits, misses);
    // hits+misses 只统计 get，每线程 OPS 次 get，共 THREADS*OPS
    uint64_t expected_gets = (uint64_t)THREADS * OPS;
    TEST_CHECK_EQ(hits + misses, expected_gets);
    std::cout << "PASS: concurrent (hits=" << hits << " misses=" << misses << ")\n";
}

int main() {
    test_basic();
    test_evict();
    test_exact_capacity();
    test_non_power_of_two_shards();
    test_t1_to_t2();
    test_ghost_adapt();
    test_stats();
    test_invalidate();
    test_concurrent();
    return TEST_SUMMARY();
}
