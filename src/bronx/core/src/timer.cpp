#include "timer.h"
#include "util.h"




namespace bronx{

BxTimer::BxTimer(uint64_t ms, std::function<void()> cb,
        BxTimerManager* manager, bool recurring)
        : ms_(ms)
        , cb_(cb)
        , manager_(manager)
        , recurring_(recurring){
    next_ = GetCurrentMs() + ms_;
}

// 只带到期时刻,用作 lower_bound 查找的哨兵
BxTimer::BxTimer(uint64_t next)
    : next_(next){
}

bool BxTimer::cancel(){
    BxTimerManager::RWMutexType::WriteLock lock(manager_->mutex_);
    // cb 还在说明没被移除过
    if(cb_){
        cb_ = nullptr;
        auto it = manager_->timers_.find(shared_from_this());
        if(it == manager_->timers_.end()){
            return false;
        }
        manager_->timers_.erase(it);
        return true;
    }
    return false;
}

bool BxTimer::refresh(){
    BxTimerManager::RWMutexType::WriteLock lock(manager_->mutex_);
    if(!cb_){
        return false;
    }
    auto it = manager_->timers_.find(shared_from_this());
    if(it == manager_->timers_.end()){
        return false;
    }
    manager_->timers_.erase(it);

    next_ = GetCurrentMs() + ms_;
    // 续期只会往后挪,不可能成为新的堆顶,所以不必通知调度器
    manager_->timers_.insert(shared_from_this());
    return true;
}

bool BxTimer::reset(uint64_t ms, bool from_now){
    bool at_front = false;
    {
        BxTimerManager::RWMutexType::WriteLock lock(manager_->mutex_);
        if(ms == ms_ && !from_now){
            return false;   // 周期没变又不从现在算,啥也不用做
        }
        if(!cb_){
            return false;
        }
        auto self = shared_from_this();
        auto it = manager_->timers_.find(self);
        if(it == manager_->timers_.end()){
            return false;
        }
        manager_->timers_.erase(it);
        // from_now 从现在起算,否则接着原起点算
        uint64_t start = 0;
        if(from_now){
            start = GetCurrentMs();
        } else {
            start = next_ - ms_;
        }
        ms_ = ms;
        next_ = start + ms_;
        // 已持有写锁,不能再调 addTimer(self),这里手动插入并判断是否成了堆顶
        auto new_it = manager_->timers_.insert(self).first;
        at_front = (new_it == manager_->timers_.begin()) && !manager_->tickled_;
        if(at_front){
            manager_->tickled_ = true;
        }
    }
    if(at_front){
        manager_->onEarliestChanged();
    }
    return true;
}

// 先比到期时刻,相同再用地址兜底(保证 set 里不同对象不相等)
bool BxTimer::Comparator::operator()(const BxTimer::ptr& lhs, const BxTimer::ptr& rhs) const{
    if(!rhs){
        return false;
    }
    if(!lhs){
        return true;
    }
    if(lhs->next_ != rhs->next_){
        return lhs->next_ < rhs->next_;
    }
    return lhs.get() < rhs.get();
}


BxTimerManager::BxTimerManager(){
    previousTime_ = GetCurrentMs();
}

BxTimerManager::~BxTimerManager(){
}

BxTimer::ptr BxTimerManager::addTimer(uint64_t ms, std::function<void()> cb,
                                  bool recurring){
    BxTimer::ptr timer(new BxTimer(ms, cb, this, recurring));
    addTimer(timer);    // 加锁插入交给重载版本
    return timer;
}

// 条件成立(weak_cond 没失效)才真正调 cb
static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb){
    std::shared_ptr<void> tmp = weak_cond.lock();
    if(tmp){
        cb();
    }
}

BxTimer::ptr BxTimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb,
                                std::weak_ptr<void> weak_cond, bool recurring){
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

uint64_t BxTimerManager::nextTimeout(){
    RWMutexType::ReadLock lock(mutex_);
    tickled_ = false;
    if(timers_.empty()){
        return ~0ull;
    }

    const BxTimer::ptr& next = *timers_.begin();
    uint64_t now_ms = GetCurrentMs();
    if(now_ms >= next->next_){
        return 0;   // 已到期
    } else {
        return next->next_ - now_ms;
    }
}

void BxTimerManager::collectExpired(std::vector<std::function<void()>>& cbs){
    uint64_t now_ms = GetCurrentMs();
    std::vector<BxTimer::ptr> expired;

    RWMutexType::WriteLock lock(mutex_);
    if(timers_.empty()){
        return;
    }

    // it 停在第一个还没到期的位置
    auto it = timers_.begin();
    if(clockWentBack(now_ms)){
        it = timers_.end();    // 时间回拨了,全当到期处理
    } else {
        if((*it)->next_ > now_ms){
            return;             // 最早的都没到期
        }
        BxTimer::ptr now_timer(new BxTimer(now_ms));
        it = timers_.lower_bound(now_timer);
        while(it != timers_.end() && (*it)->next_ == now_ms){
            ++it;               // 把"正好等于 now"的也算进去
        }
    }

    expired.insert(expired.begin(), timers_.begin(), it);
    timers_.erase(timers_.begin(), it);

    cbs.reserve(expired.size());
    for(auto& timer : expired){
        cbs.push_back(timer->cb_);
        if(timer->recurring_){
            timer->next_ = now_ms + timer->ms_;   // 循环的重排回去
            timers_.insert(timer);
        } else {
            timer->cb_ = nullptr;                  // 一次性的作废
        }
    }
}

bool BxTimerManager::hasTimer(){
    RWMutexType::ReadLock lock(mutex_);
    return !timers_.empty();
}

// 只看有没有一次性定时器。stop 时循环定时器(心跳/清理等)不算,
// 否则挂了循环定时器 stopped() 永远不成立、stop() 会卡死。
bool BxTimerManager::hasPendingOneShot(){
    RWMutexType::ReadLock lock(mutex_);
    for(const auto& t : timers_){
        if(!t->recurring_){
            return true;
        }
    }
    return false;
}

void BxTimerManager::addTimer(BxTimer::ptr timer){
    bool at_front = false;
    {
        RWMutexType::WriteLock lock(mutex_);
        auto it = timers_.insert(timer).first;
        at_front = (it == timers_.begin()) && !tickled_;
        if(at_front){
            tickled_ = true;   // 防抖,避免连续插入反复通知
        }
    }
    // 插到堆顶意味着最近到期时刻变了,通知调度器重算 epoll 超时
    if(at_front){
        onEarliestChanged();
    }
}

// 现在比上次触发还早了 1 小时以上,判定系统时间被回拨
bool BxTimerManager::clockWentBack(uint64_t now_ms){
    bool rollover = false;
    if(now_ms < previousTime_ - 1000*60*60){
        rollover = true;
    }
    previousTime_ = now_ms;
    return rollover;
}



}
