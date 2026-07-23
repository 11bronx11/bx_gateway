#include "pool.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include <cstdio>

using namespace bronx::gateway;
using clk = std::chrono::steady_clock;

// 填满用户区写 canary，free 前校验，确保没越界/UAF 破坏
static bool check_canary(void* ptr, int cls, uint8_t tag) {
    auto* p = static_cast<uint8_t*>(ptr);
    size_t n = kClsSizes[cls];
    memset(p, tag, n);
    for(size_t i = 0; i < n; ++i)
        if(p[i] != tag) return false;
    return true;
}

int main() {
    GwPool& pool = GwPool::get();
    const int NTHREADS = 8;
    const int ITERS    = 100'000;

    std::atomic<uint64_t> total_ops{0};
    std::atomic<int>      errors{0};

    auto t0 = clk::now();

    std::vector<std::thread> ts;
    ts.reserve(NTHREADS);

    for(int tid = 0; tid < NTHREADS; ++tid) {
        ts.emplace_back([&, tid]() {
            uint8_t tag = (uint8_t)(tid * 37 + 1);  // 每线程独立 tag
            for(int i = 0; i < ITERS; ++i) {
                int cls = (i + tid) % kNcls;
                void* ptr = pool.alloc(cls);
                if(!ptr) { ++errors; continue; }
                if(!check_canary(ptr, cls, tag)) {
                    ++errors;
                    pool.free(cls, ptr);
                    continue;
                }
                pool.free(cls, ptr);
            }
            pool.drainLocal();
            total_ops.fetch_add(ITERS, std::memory_order_relaxed);
        });
    }

    for(auto& t : ts) t.join();

    double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    uint64_t ops = total_ops.load();

    auto st = pool.stats();
    printf("\n=== stress_gw_pool ===\n");
    printf("threads=%d  iters/thread=%d  total=%lu  errors=%d\n",
           NTHREADS, ITERS, (unsigned long)ops, errors.load());
    printf("time=%.1f ms  throughput=%.2f M ops/s\n",
           ms, ops / ms / 1000.0);
    printf("mmaps=%lu  munmaps=%lu  tls_hits=%lu  central_hits=%lu  refills=%lu\n",
           (unsigned long)st.mmaps,    (unsigned long)st.munmaps,
           (unsigned long)st.tls_hits, (unsigned long)st.central_hits,
           (unsigned long)st.refills);

    return errors.load() > 0 ? 1 : 0;
}
