#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <sys/epoll.h>
#include "exec.h"
#include "sync.h"
#include "fiber.h"
#include "timer.h"


namespace bronx{

// I/O 协程调度器 = BxScheduler + BxTimerManager + epoll,整个 reactor 的核心。
// 思路:等 fd 读写 = 挂起协程,事件就绪再恢复。armEvent 注册关注 -> 业务协程 yield ->
// idle 阻塞在 epoll_wait -> 事件到了 fireEvent 把协程重新 post -> run() resume 它。
// 没事干就阻塞在 epoll,不空转;有事就精确唤醒。
//
// 几个坑:
//  - 一个 fd 同方向只能挂一个等待者(单读单写),重复 armEvent 会断言。
//  - wakeup 用 pipe(ET);idle 退出时接力 wakeup,否则边沿合并会让 stop 卡到超时。
//  - close/abortAll 要路由到该 fd 的 owner(BxFdCtx 记着),否则跨 manager 关连接唤不醒协程。
class BxIoManager: public BxScheduler, public BxTimerManager{
public:
    using ptr = std::shared_ptr<BxIoManager>;
    using RWMutexType = BxRwMutex;

    // 只关心读写两类
    enum Event{
        EV_NONE  = 0x0,
        EV_IN  = 0x1,     // EPOLLIN
        EV_OUT = 0x4      // EPOLLOUT
    };

private:
    // 一个 fd 的上下文:读/写各一份事件回调,加一把锁。
    struct FdContext{
        using MutexType = BxMutex;
        // 一个方向上要触发什么:在哪个调度器上、恢复哪个协程 / 调哪个 cb。
        struct EventContext{
            BxScheduler* scheduler = nullptr;
            BxFiber::ptr fiber;
            std::function<void()> cb;
        };

        EventContext& slotOf(Event event);
        void clearSlot(EventContext& ctx);
        void fireEvent(Event evnet);

        EventContext readCtx;
        EventContext writeCtx;
        int fd = 0;
        Event events = EV_NONE;    // 当前已注册的事件
        MutexType mutex;
    };

public:
    // bindWorkers=true 时把 epoll worker 线程绑核(减 cache 迁移);默认 false 保持旧行为,
    // 老调用点(gateway 的 17 处构造)零改。
    BxIoManager(size_t threads = 1, const std::string& name = "", bool bindWorkers = false);
    ~BxIoManager();

    // 注册 fd 事件,就绪时调度 cb(cb 为空则恢复当前协程)。返回 0 成功 / -1 失败。
    // Hook 的 do_io 遇 EAGAIN 时调它,顺便把本 manager 记为该 fd 的 owner。
    int armEvent(int fd, Event event, std::function<void()> cb = nullptr);

    // 删掉某事件,不触发回调。
    bool dropEvent(int fd, Event event);
    // 取消某事件,但强制触发一次它的回调(用来唤醒)。do_io 靠它 + cancel 代次识别"被取消"。
    bool abortEvent(int fd, Event event);
    // 取消 fd 上全部事件各触发一次。close 时用它唤醒所有挂在该 fd 上的协程。
    bool abortAll(int fd);

    static BxIoManager* Current();

    // 运行状态快照(给日志/metrics 看)
    struct Stats {
        size_t pendingEvents = 0;   // 待触发的 IO 事件数
        size_t activeThreads = 0;   // 活跃工作线程数
        size_t idleThreads   = 0;   // 空闲(阻塞在 epoll_wait)线程数
        size_t threadCount   = 0;   // 工作线程总数
        size_t scheduledTasks = 0;  // 待调度任务数
    };
    Stats getStats() {
        Stats s;
        s.pendingEvents  = pendingEventCount_.load();
        s.activeThreads  = getActiveThreadCount();
        s.idleThreads    = getIdleThreadCount();
        s.threadCount    = getThreadCount();
        s.scheduledTasks = getTaskCount();
        return s;
    }
    size_t getPendingEventCount() const { return pendingEventCount_.load(); }


protected:
    virtual void wakeup() override;      // 往 pipe 写一字节戳醒 epoll_wait
    virtual bool stopped() override;
    virtual void idle() override;        // 阻塞在 epoll_wait
    virtual void onEarliestChanged() override;   // 新定时器排到最前,wakeup 一下重算超时

private:
    void growSlots(size_t size);


private:
    int epfd_ = 0;
    int pipefds_[2];    // wakeup 用的管道

    std::atomic<size_t> pendingEventCount_ = {0};   // 待触发的 IO 事件数
    RWMutexType mutex_;
    std::vector<FdContext*> fdContexts_;    // 按 fd 索引
};



}