#include "stack_pool.h"
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace bronx {

// 全局统计(跨线程累计)
static std::atomic<uint64_t> g_mmaps{0};
static std::atomic<uint64_t> g_munmaps{0};
static std::atomic<uint64_t> g_reuses{0};
static std::atomic<uint64_t> g_pooled{0};

// 配置(首次 Alloc 前 Configure 定,之后只读)
static size_t g_poolCapPerSize = 8;      // 每线程每尺寸缓存上限
static bool   g_guardPage      = true;   // 是否加 PROT_NONE 保护页
static std::atomic<bool> g_configured{false};

static size_t PageSize(){
    static const size_t ps = (size_t)::sysconf(_SC_PAGESIZE);
    return ps;
}

static size_t RoundUp(size_t n, size_t align){
    return (n + align - 1) & ~(align - 1);
}

// guard page 占的字节数(0 表示关)
static size_t GuardBytes(){
    return g_guardPage ? PageSize() : 0;
}

// 由可用大小算 mmap 总长度(可用区页对齐 + guard),Alloc/Dealloc 用同一 size 故确定
static size_t TotalLen(size_t usableSize){
    return RoundUp(usableSize, PageSize()) + GuardBytes();
}

// 每线程栈池:key=可用大小,value=一组 mmap 基址
// 线程退出时析构里把残留栈全 munmap,防泄漏。
namespace {
struct ThreadStackPool {
    std::unordered_map<size_t, std::vector<void*>> freeBySize;

    ~ThreadStackPool(){
        for(auto& kv : freeBySize){
            size_t total = TotalLen(kv.first);
            for(void* base : kv.second){
                ::munmap(base, total);
                g_munmaps.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
};
} // namespace

static thread_local ThreadStackPool t_pool;

void BxStackAllocator::Configure(size_t poolCapPerSize, bool guardPage){
    bool expected = false;
    // 只认第一次;首次 Alloc 已发生后再调无效(避免运行期改布局)
    if(g_configured.compare_exchange_strong(expected, true)){
        g_poolCapPerSize = poolCapPerSize;
        g_guardPage      = guardPage;
    }
}

void* BxStackAllocator::Alloc(size_t size){
    g_configured.store(true, std::memory_order_relaxed);  // 锁定配置
    if(size == 0){
        return nullptr;
    }

    // 先查本线程池:命中直接复用,免 mmap
    auto it = t_pool.freeBySize.find(size);
    if(it != t_pool.freeBySize.end() && !it->second.empty()){
        void* base = it->second.back();
        it->second.pop_back();
        g_reuses.fetch_add(1, std::memory_order_relaxed);
        return (char*)base + GuardBytes();   // 可用区在 guard page 之上
    }

    // 池空:新 mmap 一整块 [guard | 可用]
    size_t total = TotalLen(size);
    void* base = ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(base == MAP_FAILED){
        return nullptr;
    }
    g_mmaps.fetch_add(1, std::memory_order_relaxed);

    // 低地址端一页设 PROT_NONE:x86 栈向下增长,溢出触碰它即 SIGSEGV
    if(g_guardPage){
        if(::mprotect(base, PageSize(), PROT_NONE) != 0){
            ::munmap(base, total);
            g_mmaps.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
        }
    }
    return (char*)base + GuardBytes();
}

void BxStackAllocator::Dealloc(void* usable, size_t size){
    if(!usable || size == 0){
        return;
    }
    void* base = (char*)usable - GuardBytes();   // 回推 mmap 基址

    // 池未满就入池复用,否则真 munmap
    auto& bases = t_pool.freeBySize[size];
    if(bases.size() < g_poolCapPerSize){
        bases.push_back(base);
        g_pooled.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ::munmap(base, TotalLen(size));
    g_munmaps.fetch_add(1, std::memory_order_relaxed);
}

BxStackPoolStats BxStackAllocator::Stats(){
    BxStackPoolStats s;
    s.mmaps   = g_mmaps.load(std::memory_order_relaxed);
    s.munmaps = g_munmaps.load(std::memory_order_relaxed);
    s.reuses  = g_reuses.load(std::memory_order_relaxed);
    s.pooled  = g_pooled.load(std::memory_order_relaxed);
    return s;
}

} // namespace bronx
