#pragma once

// 基于 ucontext 的栈式协程。每个协程有自己的栈和上下文,
// 可以在任意点让出(yield)、之后再切回来(resume)继续跑。

#include <ucontext.h>
#include <memory>
#include <functional>


namespace bronx{

class BxFiber: public std::enable_shared_from_this<BxFiber>{
public:
    using ptr = std::shared_ptr<BxFiber>;

    enum State {
        ACTIVE,    // 正在跑
        FINISHED,       // 跑完了,栈可以回收或复用
        SUSPENDED,      // 新建或让出后,等着被 resume
    };


private:
    // 主协程:代表线程当前的执行流本身,没有独立栈,id 为 0。
    // 只在 Current() 里线程第一次取协程时构造。
    BxFiber();

public:
    // 子协程:分配独立栈,makecontext 把入口绑到 entryTrampoline。
    BxFiber(std::function<void()> cb, size_t stacksize = 0);
    ~BxFiber();

    // 复用已结束(FINISHED)的协程对象跑新函数,省掉重新分配栈。
    void reset(std::function<void()> cb);

    // 切进来跑。从主协程切到本协程,SUSPENDED -> ACTIVE。
    void resume();
    // 让出 CPU 切回主协程。ACTIVE -> SUSPENDED(cb 已返回则 FINISHED)。
    // Hook 里 IO 没就绪时就是调这个。
    void yield();

    uint64_t getId() const { return id_; }
    State getState() const { return state_; }

    // 取当前线程正在跑的协程;线程还没有协程时,顺手建出主协程。
    static BxFiber::ptr Current();
    static void SetCurrent(BxFiber* f);
    static uint64_t CurrentId();
    static uint64_t AliveCount();

    // 所有子协程的入口蹦床:调 cb_,cb 返回后置 FINISHED 再 yield 回主协程。
    static void entryTrampoline();
    static void yieldToSched();

private:
    uint64_t id_ = 0;
    uint32_t stacksize_ = 0;
    State state_ = SUSPENDED;
    ucontext_t ctx_;
    void* stack_ = nullptr;
    std::function<void()> cb_;
};

}
