#pragma once

// 同步原语集合:信号量 + 自旋锁 + 公平锁 + 互斥量 + 读写锁 + 空锁。
// 守卫统一用标准 std::lock_guard / std::shared_lock / std::unique_lock(RAII),
// 各锁暴露标准具名要求(Lockable / SharedMutex),因而能直接被标准守卫托管;
// 每个锁保留 ::Lock / ::ReadLock / ::WriteLock 成员别名,调用点写 MutexType::Lock 即可。

#include "noncopyable.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <semaphore>
#include <thread>

namespace bronx{

// 忙等时给 CPU 的退避提示:x86 PAUSE / arm YIELD。降低超线程争用与功耗,别死转。
inline void cpu_relax(){
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    // 其它架构无对应指令,留空(编译器不会重排跨 atomic 的访存)
#endif
}


// 计数信号量。C++20 std::counting_semaphore 实现,替旧的 POSIX sem_t 封装。
// wait=acquire(减,为 0 阻塞),notify=release(加)。BxThread 启动握手用它。
class BxSemaphore: public Noncopyable{
public:
    explicit BxSemaphore(uint32_t count = 0)
        : sem_((std::ptrdiff_t)count){}

    void wait(){ sem_.acquire(); }
    void notify(){ sem_.release(); }

private:
    // 上界给足(握手/资源计数场景够用);模板上界只影响内部选型,不占运行期语义
    std::counting_semaphore<(1u << 30)> sem_;
};


// 自旋锁:atomic + 指数退避 + PAUSE。争用时忙等不睡眠,只适合极短临界区
// (日志格式化、指针交换、计数)。满足标准 Lockable,由 std::lock_guard 托管。
class BxSpinLock{
public:
    using Lock = std::lock_guard<BxSpinLock>;

    void lock(){
        // test-and-test-and-set:先只读探测,拿到再 CAS,减少总线上的写风暴
        unsigned backoff = 1;
        while(flag_.exchange(true, std::memory_order_acquire)){
            do{
                for(unsigned i = 0; i < backoff; ++i){
                    cpu_relax();
                }
                if(backoff < 1024){
                    backoff <<= 1;   // 指数退避,封顶防过度空转
                }
            } while(flag_.load(std::memory_order_relaxed));
        }
    }

    bool try_lock(){
        return !flag_.exchange(true, std::memory_order_acquire);
    }

    void unlock(){
        flag_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> flag_{false};
};

// 公平自旋锁(Ticket lock):取号排队、按序放行,FIFO 先到先得。
// 相对 BxSpinLock 的价值是"防饥饿"——高争用下 BxSpinLock 可能让某线程长期抢不到,
// Ticket lock 保证每个等待者最终按到达顺序获锁。适合争用重且要公平的短临界区。
class BxTicketLock{
public:
    using Lock = std::lock_guard<BxTicketLock>;

    void lock(){
        // 取一个号,自旋等到"叫号"轮到自己
        unsigned my = next_.fetch_add(1, std::memory_order_relaxed);
        unsigned spins = 0;
        while(serving_.load(std::memory_order_acquire) != my){
            // 短暂纯自旋后转 sched_yield:ticket 锁强制 FIFO,若持锁/前驱线程被
            // 内核抢占下线,纯自旋会让其余等待者空烧 CPU 反而挤占它上核(convoy 塌陷,
            // 线程数>核数时尤甚)。让出可让被抢占者尽快上核推进队列,优雅降级。
            if(++spins < 64){
                cpu_relax();
            } else {
                std::this_thread::yield();
            }
        }
    }

    bool try_lock(){
        unsigned cur = serving_.load(std::memory_order_acquire);
        unsigned expected = cur;
        // 只有"下一个待发号 == 当前叫号"(无人排队)才可能无等待拿到
        if(next_.load(std::memory_order_relaxed) != cur){
            return false;
        }
        return next_.compare_exchange_strong(expected, cur + 1,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed);
    }

    void unlock(){
        // 叫下一个号
        serving_.store(serving_.load(std::memory_order_relaxed) + 1,
                       std::memory_order_release);
    }

private:
    std::atomic<unsigned> next_{0};      // 下一个待分配的号
    std::atomic<unsigned> serving_{0};   // 当前正在服务的号
};


// 互斥量:争用时线程睡眠。框架里多数临界区的默认锁。直接用标准 std::mutex,
// 由 std::lock_guard / std::unique_lock 托管;保留 ::Lock 别名供调用点。
class BxMutex: public std::mutex{
public:
    using Lock = std::lock_guard<std::mutex>;
};


// 读写锁:多读单写,适合读多写少(配置、日志器、fd 表这类共享状态)。
// 直接用标准 std::shared_mutex;读守卫 shared_lock、写守卫 unique_lock
// (二者均支持手动 .unlock(),满足 reactor 里"读锁探测后升写锁"的用法)。
class BxRwMutex: public std::shared_mutex{
public:
    using ReadLock  = std::shared_lock<std::shared_mutex>;
    using WriteLock = std::unique_lock<std::shared_mutex>;
};


// 空锁:接口齐全但全是空操作。用于以模板参数关闭加锁、做单线程测试/基准对比。
class BxNullMutex{
public:
    using Lock = std::lock_guard<BxNullMutex>;
    void lock(){}
    bool try_lock(){ return true; }
    void unlock(){}
};

class BxNullRwMutex{
public:
    using ReadLock  = std::shared_lock<BxNullRwMutex>;
    using WriteLock = std::unique_lock<BxNullRwMutex>;
    void lock(){}
    bool try_lock(){ return true; }
    void unlock(){}
    void lock_shared(){}
    bool try_lock_shared(){ return true; }
    void unlock_shared(){}
};

}
