#pragma once

#include <memory>
#include <vector>
#include <list>
#include <string>
#include <functional>
#include <atomic>
#include "thread.h"
#include "fiber.h"
#include "sync.h"


namespace bronx{

// N:M 协程调度器:M 个任务(协程或裸函数)跑在 N 个工作线程上。
// 每个 worker 线程跑一个 run() 循环,从共享队列取任务 resume 执行。
// BxIoManager 继承它,把 idle 换成 epoll_wait,于是"无任务时阻塞在 epoll"
// 和"有任务时跑任务"统一进同一个 run() 循环。
class BxScheduler{
public:
    using ptr = std::shared_ptr<BxScheduler>;
    using MutexType = BxMutex;

    // 所有线程都是纯 worker,创建者不参与调度。
    BxScheduler(size_t threads = 1, const std::string& name = "");
    virtual ~BxScheduler();

    const std::string& getName() const { return name_; }

    size_t getThreadCount() const { return threadCount_; }
    size_t getActiveThreadCount() const { return activeThreadCount_.load(); }
    size_t getIdleThreadCount() const { return idleThreadCount_.load(); }
    size_t getTaskCount() {
        MutexType::Lock lock(mutex_);
        return tasks_.size();
    }

    static BxScheduler* Current();
    static BxFiber* RootFiber();

    // 起 N 个 worker 线程,各跑 run() 循环。
    // bindWorkers=true 时把 worker i 绑到 CPU 核 (i % 核数),减少 cache 迁移/跨核抖动;
    // 默认 false 保持旧行为(不绑),老调用点零改。
    void start(bool bindWorkers = false);
    // 置 stopping + wakeup 唤醒,等队列跑空、worker join。必须由非调度线程调用。
    void stop();

    // 投递任务入队,队列原本为空则 wakeup 唤醒 worker。thread 指定线程 id,-1 不限。
    template<typename FiberOrCb>
    void post(FiberOrCb f, int thread = -1){
        bool need_tickle = false;
        {
            MutexType::Lock lock(mutex_);
            need_tickle = postLocked(f, thread);
        }

        if(need_tickle){
            wakeup();
        }
    }

    template<typename InputIterator> 
    void post(InputIterator begin, InputIterator end){
        bool need_tickle = false;
        {
            MutexType::Lock lock(mutex_);
            while(begin != end){
                // -注意这里的&*begin，传递的是一个任务的指针
                need_tickle = postLocked(&*begin, -1) || need_tickle;
                ++begin;
            }
        }

        if(need_tickle){
            wakeup();
        }
    }

private:
    // 入队(调用方已持锁)。返回队列原本是否为空,用来决定要不要 wakeup。
    template<class FiberOrCb>
    bool postLocked(FiberOrCb fc, int thread = -1){
        bool need_tickle = tasks_.empty();
        ScheduleTask task(fc, thread);
        if(task.cb || task.fiber){
            tasks_.push_back(task);
        }
        return need_tickle;
    }


protected:
    // 唤醒阻塞的 worker。BxIoManager 重写为往 pipe 写一字节戳醒 epoll_wait。
    virtual void wakeup();
    // 能否终止:队列空 + 已 stopping。BxIoManager 再叠加"无待处理 IO/定时器"。
    virtual bool stopped();
    // 无任务时跑的 idle 协程。基类自旋让出,BxIoManager 重写为阻塞在 epoll_wait。
    virtual void idle();

    // 调度核心:循环取任务 resume,无任务就切到 idle。整个 reactor 的主循环。
    void run();

    bool hasIdleWorker() const { return idleThreadCount_ > 0; }

private:
    // 一个任务:要么是协程,要么是裸函数;thread 可指定跑在哪个线程。
    struct ScheduleTask{
        BxFiber::ptr fiber;
        std::function<void()> cb;
        int thread;

        ScheduleTask(BxFiber::ptr f, int thr)
            : fiber(f)
            , thread(thr){
        }

        // 指针版:swap 进来,省一次引用计数增减
        ScheduleTask(BxFiber::ptr* f, int thr)
            : thread(thr){
            fiber.swap(*f);
        }

        ScheduleTask(std::function<void()> f, int thr)
            : cb(f)
            , thread(thr){
        }

        ScheduleTask(std::function<void()>* f, int thr)
            : thread(thr){
            cb.swap(*f);
        }

        ScheduleTask()
            : thread(-1){
        }

        void reset(){
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }
    };

private:
    MutexType mutex_;
    std::vector<BxThread::ptr> threads_;   // worker 线程池
    std::list<ScheduleTask> tasks_;        // 任务队列
    std::string name_;

    size_t threadCount_ = 0;
    std::atomic<size_t> activeThreadCount_ = {0};
    std::atomic<size_t> idleThreadCount_ = {0};
    std::vector<int> threadIds_;

    // stop() 在 caller 线程写,run()/stopped() 在 worker 线程读,必须用 atomic。
    std::atomic<bool> stopping_{false};
};






}
