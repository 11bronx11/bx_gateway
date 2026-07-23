#include "pool.h"
#include <sys/mman.h>
#include <cstring>
#include <new>

namespace bronx {
namespace gateway {

thread_local Bin GwPool::t_bins[kNcls];

GwPool& GwPool::get() {
    static GwPool inst;
    return inst;
}

GwPool::GwPool() {}

// 块内 next 指针存在 block_base+8（用户区开头 8 字节）
static inline void*& blockNext(void* base) {
    return *reinterpret_cast<void**>(static_cast<char*>(base) + 8);
}

// 把 Span 挂进 partial 双链头
void GwPool::linkPartial(Slab& sl, Span* sp) {
    sp->prev = nullptr;
    sp->next = sl.partial;
    if(sl.partial) sl.partial->prev = sp;
    sl.partial = sp;
}

// 从 partial 双链摘掉 Span
void GwPool::unlinkPartial(Slab& sl, Span* sp) {
    if(sp->prev) sp->prev->next = sp->next;
    else         sl.partial = sp->next;
    if(sp->next) sp->next->prev = sp->prev;
    sp->prev = sp->next = nullptr;
}

// 锁外：mmap + 切块，返回未挂链的 Span，由调用方在锁内 linkPartial
Span* GwPool::allocSpan(int cls) {
    int    n   = kSpanBlocks[cls];
    size_t bsz = kClsSizes[cls] + 8;  // 每 block = Span*(8B) + 用户区
    size_t total = (size_t)n * bsz;

    Span* sp = new (std::nothrow) Span();
    if(!sp) return nullptr;

    void* raw = mmap(nullptr, total, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if(raw == MAP_FAILED) {
        delete sp;
        return nullptr;
    }

    sp->base      = raw;
    sp->total     = total;
    sp->cls       = cls;
    sp->nblocks   = n;
    sp->used      = 0;
    sp->free_head = nullptr;
    sp->prev = sp->next = nullptr;

    m_st.mmaps.fetch_add(1, std::memory_order_relaxed);

    // 切成 n 个 block：block_base 处存 Span*，串成 Span 内空闲链
    char* p = static_cast<char*>(raw);
    for(int i = 0; i < n; ++i) {
        *reinterpret_cast<Span**>(p) = sp;
        blockNext(p) = sp->free_head;
        sp->free_head = p;
        p += bsz;
    }
    return sp;
}

// TLS 空，从中心层批量取块。
// 修复1: 锁外 mmap，不持 spinlock 等系统调用
// 修复2: thread_local TlsGuard 注册线程退出自动 drain
// 修复3: 此路径不计入 tls_hits（通过中心层，不是 TLS 命中）
void* GwPool::fill_alloc(int cls) {
    thread_local TlsGuard tls_guard;  // 线程退出时析构 → drainLocal()

    Slab& sl = m_slabs[cls];
    Bin&  b  = t_bins[cls];

    // 第一次加锁：仅判断是否需要 mmap，立即释放
    bool need_span;
    {
        BxSpinLock::Lock lk(sl.lk);
        need_span = (sl.partial == nullptr);
    }

    // 锁外 mmap + 切块（不持 spinlock）
    if(need_span) {
        Span* fresh = allocSpan(cls);
        if(!fresh) return nullptr;
        BxSpinLock::Lock lk(sl.lk);
        linkPartial(sl, fresh);
        m_st.refills.fetch_add(1, std::memory_order_relaxed);
    }

    // 第二次加锁：批量取块到 TLS Bin
    int got = 0;
    {
        BxSpinLock::Lock lk(sl.lk);

        if(!sl.partial) return nullptr;  // mmap 失败或极罕见竞争后仍空

        if(!need_span)
            m_st.central_hits.fetch_add(1, std::memory_order_relaxed);

        int batch = b.max / 2;
        if(batch < 1) batch = 1;

        for(; got < batch; ++got) {
            Span* sp = sl.partial;
            if(!sp) break;

            void* base = sp->free_head;
            sp->free_head = blockNext(base);
            ++sp->used;

            if(!sp->free_head) unlinkPartial(sl, sp);

            blockNext(base) = b.head;
            b.head = base;
            ++b.cnt;
        }

        if(b.max < 256) ++b.max;
    }

    if(got == 0) return nullptr;

    void* base = b.head;
    b.head = blockNext(base);
    --b.cnt;
    return static_cast<char*>(base) + 8;
}

// TLS 超高水位，把前半段归还中心层。每块按块头 Span* 各回各家，
// Span 从满变非满则挂回 partial，used 归 0 则 munmap。
void GwPool::drain(int cls, Bin& b) {
    int give = b.max / 2;
    if(give < 1) give = 1;
    if(give > b.cnt) give = b.cnt;

    Slab& sl = m_slabs[cls];
    BxSpinLock::Lock lk(sl.lk);

    for(int i = 0; i < give; ++i) {
        void* base = b.head;
        b.head = blockNext(base);
        --b.cnt;

        Span* sp = *reinterpret_cast<Span**>(base);
        bool wasFull = (sp->free_head == nullptr);

        // 块插回 Span 内空闲链
        blockNext(base) = sp->free_head;
        sp->free_head = base;
        --sp->used;

        if(sp->used == 0) {
            // 整 Span 空了，摘链 + munmap
            if(!wasFull) unlinkPartial(sl, sp);
            // wasFull 且 used==0 只可能 nblocks==1，那种 class 不存在，保险起见也处理
            munmap(sp->base, sp->total);
            m_st.munmaps.fetch_add(1, std::memory_order_relaxed);
            delete sp;
        } else if(wasFull) {
            // 满 Span 现在有空闲了，挂回 partial
            linkPartial(sl, sp);
        }
    }
}

// 把当前线程 TLS bins 全还回中心层，Span used==0 则 munmap+delete
void GwPool::drainLocal() {
    for(int cls = 0; cls < kNcls; ++cls) {
        Bin& b = t_bins[cls];
        if(!b.head) continue;

        Slab& sl = m_slabs[cls];
        BxSpinLock::Lock lk(sl.lk);

        while(b.head) {
            void* base = b.head;
            b.head = blockNext(base);
            --b.cnt;

            Span* sp = *reinterpret_cast<Span**>(base);
            bool wasFull = (sp->free_head == nullptr);
            blockNext(base) = sp->free_head;
            sp->free_head   = base;
            --sp->used;

            if(sp->used == 0) {
                if(!wasFull) unlinkPartial(sl, sp);
                munmap(sp->base, sp->total);
                m_st.munmaps.fetch_add(1, std::memory_order_relaxed);
                delete sp;
            } else if(wasFull) {
                linkPartial(sl, sp);
            }
        }
    }
}

// 析构：先清当前线程 TLS，再清 partial 链上的残余 Span（进程退出场景）
GwPool::~GwPool() {
    drainLocal();
    for(int cls = 0; cls < kNcls; ++cls) {
        Slab& sl = m_slabs[cls];
        Span* sp = sl.partial;
        while(sp) {
            Span* next = sp->next;
            munmap(sp->base, sp->total);
            m_st.munmaps.fetch_add(1, std::memory_order_relaxed);
            delete sp;
            sp = next;
        }
        sl.partial = nullptr;
    }
}

PoolStats GwPool::stats() const {
    return {
        m_st.mmaps.load(std::memory_order_relaxed),
        m_st.munmaps.load(std::memory_order_relaxed),
        m_st.tls_hits.load(std::memory_order_relaxed),
        m_st.central_hits.load(std::memory_order_relaxed),
        m_st.refills.load(std::memory_order_relaxed),
    };
}

} // namespace gateway
} // namespace bronx
