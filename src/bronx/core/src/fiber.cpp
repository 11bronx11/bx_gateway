#include "fiber.h"
#include "log.h"
#include "macro.h"
#include "config.h"
#include "exec.h"
#include "stack_pool.h"
#include <atomic>
#include <cstdlib>



namespace bronx{

static BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

static std::atomic<uint64_t> s_fiber_id {0};      // 全局协程 id 分配
static std::atomic<uint64_t> s_fiber_count {0};   // 存活协程数

static thread_local BxFiber* t_fiber = nullptr;         // 本线程当前协程
static thread_local BxFiber::ptr t_thread_fiber = nullptr;  // 本线程主协程


static BxConfigVar<uint32_t>::ptr g_fiber_stack_size =
    BxConfig::Lookup<uint32_t>("fiber.stack_size", 128 * 1024, "fiber stack size");

// 每线程每尺寸缓存的空闲栈数上限(池化复用,免反复 mmap)
static BxConfigVar<uint32_t>::ptr g_fiber_stack_pool =
    BxConfig::Lookup<uint32_t>("fiber.stack_pool_size", 8, "per-thread cached free stacks per size");

// 是否给每块栈加 guard page(栈溢出即 SIGSEGV,而非静默踩内存)
static BxConfigVar<bool>::ptr g_fiber_guard_page =
    BxConfig::Lookup<bool>("fiber.guard_page", true, "protect each fiber stack with a guard page");


// 栈分配器:mmap 栈池 + guard page(见 stack_pool.h)。Alloc/Dealloc 接口同旧 Malloc 版,
// BxFiber 无感切换。首次分配前用配置项定池容量/保护页开关。
using StackAllocator = BxStackAllocator;

static bool initStackPool(){
    BxStackAllocator::Configure(g_fiber_stack_pool->getValue(),
                                g_fiber_guard_page->getValue());
    return true;
}


// 主协程:不分配栈,直接接管当前执行流,id=0。
BxFiber::BxFiber(){
    state_ = ACTIVE;
    SetCurrent(this);
    if(getcontext(&ctx_)){
        BRONX_ASSERT2(false, "getcontext");
    }
    id_ = 0;
    ++s_fiber_count;
}

BxFiber::BxFiber(std::function<void()> cb, size_t stacksize)
    : id_(++s_fiber_id)
    , cb_(cb){
    ++s_fiber_count;
    // 首次建子协程时按配置锁定栈池(池容量/保护页),只跑一次
    static bool once = initStackPool();
    (void)once;
    stacksize_ = stacksize ? stacksize : g_fiber_stack_size->getValue();
    stack_ = StackAllocator::Alloc(stacksize_);
    BRONX_ASSERT2(stack_, "fiber stack alloc failed");
    if(getcontext(&ctx_)){
        BRONX_ASSERT2(false, "getcontext");
    }
    // 非对称协程:不用 uc_link 自动跳转,切换全由调度器控制。
    ctx_.uc_link = nullptr;
    ctx_.uc_stack.ss_sp = stack_;
    ctx_.uc_stack.ss_size = stacksize_;

    makecontext(&ctx_, entryTrampoline, 0);
}

BxFiber::~BxFiber(){
    --s_fiber_count;
    if(stack_){
        // 子协程:只有结束或就绪态才允许析构
        BRONX_ASSERT(state_ == FINISHED || state_ == SUSPENDED);
        StackAllocator::Dealloc(stack_, stacksize_);
    } else {
        // 主协程:没有 cb,应处于运行态
        BRONX_ASSERT(!cb_);
        BRONX_ASSERT(state_ == ACTIVE);
        if(this == t_fiber){
            SetCurrent(nullptr);
        }
    }
}

// 复用协程栈:只有非运行态才能重置
void BxFiber::reset(std::function<void()> cb){
    BRONX_ASSERT(stack_);
    BRONX_ASSERT(state_ != ACTIVE);

    cb_ = cb;
    if(getcontext(&ctx_)){
        BRONX_ASSERT2(false, "getcontext");
    }
    ctx_.uc_link = nullptr;
    ctx_.uc_stack.ss_sp = stack_;
    ctx_.uc_stack.ss_size = stacksize_;

    makecontext(&ctx_, &entryTrampoline, 0);
    state_ = SUSPENDED;
}

void BxFiber::resume(){
    BRONX_ASSERT(state_ == SUSPENDED);
    SetCurrent(this);
    state_ = ACTIVE;
    // 工作线程的调度协程就是它的主协程,子协程统一跟主协程换栈。
    if(swapcontext(&(t_thread_fiber->ctx_), &ctx_)){
        BRONX_ASSERT2(false, "swapcontext");
    }
}

void BxFiber::yield(){
    BRONX_ASSERT(state_ == ACTIVE || state_ == FINISHED);
    SetCurrent(t_thread_fiber.get());
    if(state_ != FINISHED){
        state_ = SUSPENDED;
    }
    if(swapcontext(&ctx_, &(t_thread_fiber->ctx_))){
        BRONX_ASSERT2(false, "swapcontext");
    }
}

void BxFiber::yieldToSched(){
    Current()->yield();
}


BxFiber::ptr BxFiber::Current(){
    if(t_fiber){
        return t_fiber->shared_from_this();
    }
    // 线程还没进过协程世界,先建主协程
    BxFiber::ptr main_fiber(new BxFiber);
    BRONX_ASSERT(t_fiber == main_fiber.get());
    t_thread_fiber = main_fiber;
    return t_fiber->shared_from_this();
}

void BxFiber::SetCurrent(BxFiber* f){
    t_fiber = f;
}

uint64_t BxFiber::CurrentId(){
    if(t_fiber){
        return t_fiber->getId();
    }
    return 0;
}

uint64_t BxFiber::AliveCount(){
    return s_fiber_count;
}

void BxFiber::entryTrampoline(){
    BxFiber::ptr cur = Current();
    BRONX_ASSERT(cur);
    try{
        cur->cb_();
        cur->cb_ = nullptr;
        cur->state_ = FINISHED;
    } catch(std::exception& e) {
        BRONX_LOG_ERROR(g_logger) << "BxFiber Except: " << e.what()
            << " fiber_id=" << cur->getId() << std::endl
            << BacktraceToString();
    } catch(...){
        BRONX_LOG_ERROR(g_logger) << "BxFiber Except"
            << " fiber_id=" << cur->getId() << std::endl
            << BacktraceToString();
    }

    // 结束后让出。先取裸指针再 reset,否则智能指针在 yield 后无法析构。
    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->yield();

    BRONX_ASSERT2(false, "Never reach fiber_id=" + std::to_string(raw_ptr->getId()));
}


}
