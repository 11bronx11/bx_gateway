#pragma once
#include <atomic>

#include <set>
#include <vector>
#include <memory>
#include <functional>
#include "sync.h"


namespace bronx{

class BxTimerManager;

// 一个定时器(一次性或循环)。一般不直接 new,由 BxTimerManager::addTimer 建。
// next_ 是绝对到期时刻(now + ms_);循环的到期后按 ms_ 自动重排。
class BxTimer: public std::enable_shared_from_this<BxTimer>{
    friend class BxTimerManager;

public:
    using ptr = std::shared_ptr<BxTimer>;

    bool cancel();
    bool refresh();                            // 续期:下次触发推到 now+ms_
    bool reset(uint64_t ms, bool from_now);    // 改周期并重排

private:
    BxTimer(uint64_t ms, std::function<void()> cb,
          BxTimerManager* manager, bool recurring);
    BxTimer(uint64_t next);

private:
    uint64_t ms_ = 0;          // 周期(毫秒)
    uint64_t next_ = 0;        // 绝对到期时刻
    std::function<void()> cb_;
    BxTimerManager* manager_ = nullptr;
    bool recurring_ = false;

private:
    // 按到期时刻排序
    struct Comparator{
        bool operator()(const BxTimer::ptr& lhs, const BxTimer::ptr& rhs) const;
    };
};


// 定时器管理器(BxIoManager 的基类之一)。用一个按到期时刻排序的 set 当时间堆:
// 给 epoll_wait 提供超时值,到期时批量取回调交调度器。定时不额外起线程,复用 epoll 超时。
class BxTimerManager{
public:
    friend class BxTimer;

public:
    using RWMutexType = BxRwMutex;

    BxTimerManager();
    virtual ~BxTimerManager();

    // 加定时器,返回句柄(可 cancel/reset)。插到堆顶会 onEarliestChanged 通知 manager。
    BxTimer::ptr addTimer(uint64_t ms, std::function<void()> cb,
                        bool recurring = false);

    // 条件定时器:weak_cond 失效(对象已析构)就不触发。用于 do_io 超时——连接没了别触发。
    BxTimer::ptr addConditionTimer(uint64_t ms, std::function<void()> cb,
                                 std::weak_ptr<void> weak_cond,
                                 bool recurring = false);

    // 距最近一个到期还剩多少毫秒,无定时器返回 ~0ull。给 epoll_wait 当超时。
    uint64_t nextTimeout();

    // 取出所有到期回调(传出 cbs),循环的顺带重排。idle 在 epoll 返回后调它。
    void collectExpired(std::vector<std::function<void()>>& cbs);

    bool hasTimer();
    bool hasPendingOneShot();   // 还有没有一次性定时器(优雅停止判据)

protected:
    // 新定时器排到了最前,通知调度器重算 epoll 超时
    virtual void onEarliestChanged() = 0;

private:
    void addTimer(BxTimer::ptr timer);
    bool clockWentBack(uint64_t now_ms);   // 检测系统时间回拨

private:
    RWMutexType mutex_;
    // 时间堆:最早到期的排头。必须显式传 Comparator 按 next_ 排,否则 set 默认按
    // shared_ptr 的指针地址排,时序全乱(IO 超时/sleep 全失效)——踩过的坑。
    std::set<BxTimer::ptr, BxTimer::Comparator> timers_;
    // 防抖:避免频繁重复通知。nextTimeout 读锁下写、addTimer 写锁下写,非互斥,得用 atomic。
    std::atomic<bool> tickled_{false};
    uint64_t previousTime_ = 0;    // 上次触发时刻,配合回拨检测
};


}