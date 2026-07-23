#pragma once

// 协程栈分配器,替掉裸 malloc。干两件事,一是栈复用,协程结束归还的栈不 munmap
// 而是进每线程的池子(thread_local 所以不用加锁),下个协程直接拿来用,省掉高并发下
// 反复大块 mmap 和首次触页。二是每块栈低地址端垫一页 PROT_NONE 当 guard,栈向下写溢出
// 一碰就 SIGSEGV,不会闷声踩坏邻居内存。
//
// mmap 出来的一整块是 [guard 页 | 可用栈],基址就是 guard 页,可用区 ss_sp 从基址往后
// 一页开始。ucontext 的栈不能搬,所以做不了 Go 那种自动扩栈,这里只管池化和溢出报警。

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bronx {

// 栈池运行统计(可观测 + 测试断言用)
struct BxStackPoolStats {
    uint64_t mmaps   = 0;   // 实际 mmap 次数(池未命中才 +1)
    uint64_t munmaps = 0;   // 实际 munmap 次数(池满或超尺寸才 +1)
    uint64_t reuses  = 0;   // 池命中复用次数
    uint64_t pooled  = 0;   // 归还入池次数
};

class BxStackAllocator {
public:
    // 分配 size 字节可用栈,返回可用区起址(guard page 之上)。失败返回 nullptr。
    static void* Alloc(size_t size);
    // 归还栈(usable=Alloc 的返回值,size 同分配值)。入池或 munmap。
    static void  Dealloc(void* usable, size_t size);

    // 进程级调优,须在首次 Alloc 前调用(幂等,只认第一次):
    //  poolCapPerSize=每线程每尺寸缓存上限;guardPage=是否加保护页。
    static void  Configure(size_t poolCapPerSize, bool guardPage);

    // 统计快照(跨线程累计,relaxed 读)
    static BxStackPoolStats Stats();
};

} // namespace bronx
