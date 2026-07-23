#include "exec.h"
#include "log.h"
#include "macro.h"
#include "util.h"
#include "io_hook.h"
#include <thread>



namespace bronx{

static BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

static thread_local BxScheduler* t_scheduler = nullptr;      // 本线程所属调度器
static thread_local BxFiber* t_scheduler_fiber = nullptr;    // 本线程调度协程


// 创建者不参与调度,只管 start/stop 和等停;threads 就是 worker 数。
BxScheduler::BxScheduler(size_t threads, const std::string& name)
    : name_(name){

    BRONX_ASSERT(threads > 0);
    BxThread::SetName(name_);
    threadCount_ = threads;
}

BxScheduler::~BxScheduler(){
    BRONX_ASSERT(stopping_);
    if(t_scheduler == Current()){
        t_scheduler = nullptr;
    }
}

BxScheduler* BxScheduler::Current(){
    return t_scheduler;
}

BxFiber* BxScheduler::RootFiber(){
    return t_scheduler_fiber;
}

void BxScheduler::start(bool bindWorkers){
    MutexType::Lock lock(mutex_);
    if(stopping_){
        BRONX_LOG_ERROR(g_logger) << "Schduler is stopped";
        return;
    }
    BRONX_ASSERT(threads_.empty());
    threads_.resize(threadCount_);
    unsigned ncpu = std::thread::hardware_concurrency();
    for(size_t i = 0; i < threadCount_; ++i){
        std::string tname = name_ + "_" + std::to_string(i);
        // 无参 lambda(非 std::bind):bind 结果会吞多余实参,同时可转 void()/void(stop_token)
        // 两个 BxThread 重载 → 歧义;lambda 无此问题。
        auto body = [this]{ run(); };
        if(bindWorkers && ncpu > 0){
            // worker i 绑到核 i%ncpu(best-effort:BxThread 内设不上只告警)
            BxThreadOptions opts;
            opts.cpuAffinity = (int)(i % ncpu);
            threads_[i].reset(new BxThread(body, tname, opts));
        } else {
            threads_[i].reset(new BxThread(body, tname));
        }
        // BxThread 构造里有信号量,返回时线程已跑起来,拿 id 是安全的
        threadIds_.push_back(threads_[i]->getId());
    }
}


// stopping + 队列空 + 无活跃线程,才算真停了
bool BxScheduler::stopped(){
    MutexType::Lock lock(mutex_);
    return stopping_ && tasks_.empty() && (activeThreadCount_ == 0);
}

// 基类空实现,派生类(BxIoManager)覆写
void BxScheduler::wakeup(){
}


void BxScheduler::stop(){
    if(stopped()){
        return;
    }
    stopping_ = true;

    // 发起 stop 的线程不能是调度线程自己
    BRONX_ASSERT(Current() != this);

    for(size_t i = 0; i < threadCount_; ++i){
        wakeup();
    }

    std::vector<BxThread::ptr> thrs;
    {
        MutexType::Lock lock(mutex_);
        thrs.swap(threads_);
    }
    for(auto& thr : thrs){
        thr->join();
    }
}

// 基类 idle:没停就一直让出
void BxScheduler::idle(){
    while(!stopped()){
        BxFiber::yieldToSched();
    }
}


void BxScheduler::run(){
    // 本线程开 hook,退出时还原(RAII)
    bool old_hook = is_hook_enable();
    set_hook_enable(true);
    struct HookGuard {
        bool prev;
        ~HookGuard(){ set_hook_enable(prev); }
    } hook_guard{old_hook};

    // 本线程的主协程即调度协程
    t_scheduler = this;
    t_scheduler_fiber = BxFiber::Current().get();

    BxFiber::ptr idle_fiber(new BxFiber(std::bind(&BxScheduler::idle, this)));
    BxFiber::ptr cb_fiber = nullptr;   // 复用同一个协程壳跑不同的 cb

    ScheduleTask task;
    while(true){
        task.reset();
        bool tickle_me = false;
        {
            MutexType::Lock lock(mutex_);
            auto it = tasks_.begin();
            while(it != tasks_.end()){
                // 任务指定了别的线程,留给它,顺手 wakeup
                if((it->thread != -1) && (it->thread != GetThreadId())){
                    tickle_me = true;
                    ++it;
                    continue;
                }

                BRONX_ASSERT(it->fiber || it->cb);

                // 竞态规避:hook 的 do_io 是"先 armEvent 再 yield",高并发下事件可能
                // 在协程 yield 之前就被别的线程触发并 post 回来,此时状态还是 ACTIVE。
                // 直接跳过,等它下一轮再取,免得两个线程并发 resume 同一协程。
                if(it->fiber && it->fiber->getState() == BxFiber::ACTIVE) {
                    ++it;
                    continue;
                }

                task = *it;
                tasks_.erase(it++);
                ++activeThreadCount_;
                break;
            }
            // 队列里还有剩,叫醒别的线程一起消化
            tickle_me |= (it != tasks_.end());
        }

        if(tickle_me){
            wakeup();
        }

        if(task.fiber){
            task.fiber->resume();       // 返回即代表这轮跑完(结束或中途 yield)
            --activeThreadCount_;
            if(stopping_){
                wakeup();
            }
            task.reset();
        } else if(task.cb){
            if(cb_fiber){
                cb_fiber->reset(task.cb);
            } else {
                cb_fiber.reset(new BxFiber(task.cb));
            }
            cb_fiber->resume();
            --activeThreadCount_;
            if(stopping_){
                wakeup();
            }
            cb_fiber.reset();
            task.reset();
        } else {
            // 队列空,跑 idle。idle 正常会反复 yield 不终止;它一旦 FINISHED 说明该停了。
            if(idle_fiber->getState() == BxFiber::FINISHED){
                // 退出前再 wakeup 一次:wakeup pipe 是 ET,stop() 的多次写会被合并成
                // 一个边沿只唤醒一个 worker,其余的收不到边沿要等超时兜底。这里逐个接力唤醒。
                wakeup();
                break;
            }
            ++idleThreadCount_;
            idle_fiber->resume();
            --idleThreadCount_;
        }
    }
}





}
