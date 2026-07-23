#pragma once

// 分配器入口，内联 sizeClass 查表直接走 GwPool，热路径零间接层。
// 非标大小 fallback ::operator new/delete。setAllocator 仅测试/benchmark 用。

#include "pool.h"
#include <cstddef>
#include <functional>
#include <new>

namespace bronx {
namespace gateway {

class MemAlloc {
public:
    static void* alloc(size_t size) {
        int cls = sizeClass(size);
        if(cls >= 0) {
            void* ptr = GwPool::get().alloc(cls);
            if(!ptr) throw std::bad_alloc();
            return ptr;
        }
        return s_alloc ? s_alloc(size) : ::operator new(size);
    }

    static void free(void* ptr, size_t size) {
        if(!ptr) return;
        int cls = sizeClass(size);
        if(cls >= 0) { GwPool::get().free(cls, ptr); return; }
        if(s_free) s_free(ptr, size);
        else       ::operator delete(ptr);
    }

    // 启动时注入一次，仅对非标大小生效，线程不安全（应在任何 alloc 前设置）
    static void setAllocator(std::function<void*(size_t)>       alloc_fn,
                              std::function<void(void*, size_t)> free_fn) {
        s_alloc = std::move(alloc_fn);
        s_free  = std::move(free_fn);
    }

private:
    inline static std::function<void*(size_t)>       s_alloc;
    inline static std::function<void(void*, size_t)> s_free;
};

} // namespace gateway
} // namespace bronx
