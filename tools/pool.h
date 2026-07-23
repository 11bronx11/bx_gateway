#pragma once

// 网关专用三层内存池，覆盖 InBuf 的 4K-64K 分配热点。
// TLS Bin(零锁) → Slab(per-class spinlock) → PageHeap(mmap)
// block 布局：[Span*(8B) | 用户区(cls_size B)]，free 时从 ptr-8 读回 Span*。

#include "sync.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bronx {
namespace gateway {

static constexpr int    kNcls = 5;
static constexpr size_t kClsSizes[kNcls]  = {4096, 8192, 16384, 32768, 65536};
static constexpr int    kSpanBlocks[kNcls] = {16, 8, 4, 4, 2};

// size → class 下标，不在表里返回 -1
inline int sizeClass(size_t sz) {
    for(int i = 0; i < kNcls; ++i)
        if(kClsSizes[i] == sz) return i;
    return -1;
}

// mmap 出来的一整块，切成 N 个 block。自带 intrusive 空闲链和 used 计数，
// used 归 0 即整块空闲，可 munmap 还给 OS（O(1) 判定，不用扫全局链）。
struct Span {
    void*  base;      // mmap 基址
    size_t total;     // mmap 总字节数
    int    cls;
    int    nblocks;
    int    used;      // 已分出去的块数，==0 时整 Span 空
    void*  free_head; // Span 内空闲块链头（指向 block_base，next 存 block_base+8）
    Span*  prev;      // Slab partial 双链
    Span*  next;
};

// 每线程每 class 一个，零锁热路径
struct Bin {
    void* head = nullptr;  // 链表头，指向 block_base
    int   cnt  = 0;
    int   max  = 16;       // 高水位，慢启动上限 256
};

// 中心层，每 class 独立锁，alignas(64) 防 false sharing。
// 只挂"还有空闲块的" Span（partial 双链）；used==0 的 Span 已 munmap 不留。
struct alignas(64) Slab {
    BxSpinLock lk;
    Span*      partial = nullptr;  // 有空闲块的 Span 双链头
};

// relaxed 原子统计，快照不保证强一致
struct PoolStats {
    uint64_t mmaps;
    uint64_t munmaps;
    uint64_t tls_hits;
    uint64_t central_hits;
    uint64_t refills;
};

class GwPool {
public:
    static GwPool& get();
    ~GwPool();

    // 热路径内联，TLS 命中零锁
    void* alloc(int cls) {
        Bin& b = t_bins[cls];
        if(b.head) {
            void* base = b.head;
            b.head = *reinterpret_cast<void**>(static_cast<char*>(base) + 8);
            --b.cnt;
            m_st.tls_hits.fetch_add(1, std::memory_order_relaxed);
            return static_cast<char*>(base) + 8;
        }
        return fill_alloc(cls);
    }

    void free(int cls, void* ptr) {
        void* base = static_cast<char*>(ptr) - 8;
        Bin& b = t_bins[cls];
        *reinterpret_cast<void**>(static_cast<char*>(base) + 8) = b.head;
        b.head = base;
        ++b.cnt;
        if(b.cnt > b.max) drain(cls, b);
    }

    PoolStats stats() const;

    // 把当前线程 TLS bins 全部还回中心层（线程退出前或测试尾部调用）
    void drainLocal();

private:
    // 线程退出时触发 drainLocal，自动回收 TLS bins
    struct TlsGuard {
        ~TlsGuard() { GwPool::get().drainLocal(); }
    };

    GwPool();

    void* fill_alloc(int cls);          // TLS 空，从中心层批量取块
    void  drain(int cls, Bin& b);       // TLS 超高水位，把后半段归还中心层
    Span* allocSpan(int cls);           // 锁外：mmap + 切块，不挂 partial 链

    // 以下两个都在 Slab 锁内操作 partial 双链
    void  linkPartial(Slab& sl, Span* sp);
    void  unlinkPartial(Slab& sl, Span* sp);

    alignas(64) Slab m_slabs[kNcls];

    struct alignas(64) {
        std::atomic<uint64_t> mmaps{0};
        std::atomic<uint64_t> munmaps{0};
        std::atomic<uint64_t> tls_hits{0};
        std::atomic<uint64_t> central_hits{0};
        std::atomic<uint64_t> refills{0};
    } m_st;

    static thread_local Bin t_bins[kNcls];
};

} // namespace gateway
} // namespace bronx
