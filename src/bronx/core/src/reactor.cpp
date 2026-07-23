#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include "reactor.h"
#include "log.h"
#include "macro.h"
#include "fd_context.h"


static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");



namespace bronx{

static void ClearFdOwnerIfIdle(int fd, BxIoManager::Event events){
    if(events != BxIoManager::EV_NONE){
        return;
    }
    if(auto ctx = FdMgr::GetInstance()->get(fd)){
        ctx->setIOManager(nullptr);
    }
}

BxIoManager::BxIoManager(size_t thread, const std::string& name, bool bindWorkers)
    :BxScheduler(thread, name){
    epfd_ = epoll_create(42);
    BRONX_ASSERT(epfd_ > 0);

    // wakeup 管道:写端投一字节,epoll_wait 监听读端就被戳醒
    int rt = pipe(pipefds_);
    BRONX_ASSERT(!rt);

    epoll_event event;
    memset(&event, 0, sizeof(epoll_event));
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = pipefds_[0];

    // 两端都设非阻塞:读端配合 ET,写端防 wakeup 风暴时阻塞调度线程
    rt = fcntl(pipefds_[0], F_GETFL, 0);
    BRONX_ASSERT(rt >= 0);
    rt = fcntl(pipefds_[0], F_SETFL, rt | O_NONBLOCK);
    BRONX_ASSERT(!rt)

    rt = fcntl(pipefds_[1], F_GETFL, 0);
    BRONX_ASSERT(rt >= 0);
    rt = fcntl(pipefds_[1], F_SETFL, rt | O_NONBLOCK);
    BRONX_ASSERT(!rt)

    rt = epoll_ctl(epfd_, EPOLL_CTL_ADD, pipefds_[0], &event);
    BRONX_ASSERT(!rt);

    growSlots(32);
    start(bindWorkers);
}

BxIoManager::~BxIoManager(){
    stop();
    close(epfd_);
    close(pipefds_[0]);
    close(pipefds_[1]);
    for(auto& i : fdContexts_){
        if(i){
            delete i;
        }
    }
}

// 只增不减:fd 直接作数组下标,新槽位建好 FdContext
void BxIoManager::growSlots(size_t size){
    if(size <= fdContexts_.size()){
        return;
    }
    size_t old_size = fdContexts_.size();
    fdContexts_.resize(size);

    for(size_t i = old_size; i < size; ++i){
        if(!fdContexts_[i]){
            fdContexts_[i] = new FdContext;
            fdContexts_[i]->fd = i;
        }
    }
}

int BxIoManager::armEvent(int fd, Event event, std::function<void()> cb){
    if(fd < 0){
        return -1;
    }
    FdContext* fd_ctx = nullptr;
    // fd 在范围内读锁就够;要扩容才升写锁
    RWMutexType::ReadLock lockR(mutex_);
    if((int)fdContexts_.size() > fd){
        fd_ctx = fdContexts_[fd];
        lockR.unlock();
    } else {
        lockR.unlock();
        RWMutexType::WriteLock lockW(mutex_);
        if((int)fdContexts_.size() <= fd){
            // 写锁下复查 + 只增不减,否则并发扩容会把别的线程刚注册的 fd 缩掉
            growSlots((size_t)fd * 3 / 2 + 1);
        }
        fd_ctx = fdContexts_[fd];
    }

    FdContext::MutexType::Lock lock(fd_ctx->mutex);
    // 同一 fd 同方向不能重复注册
    if(fd_ctx->events & event){
        BRONX_LOG_ERROR(g_logger) << "armEvent assert fd=" << fd
                                  << " event=" << (EPOLL_EVENTS)event
                                  << " fd_ctx.event=" << (EPOLL_EVENTS)fd_ctx->events;
        BRONX_ASSERT(!(fd_ctx->events & event));
    }
    // 已有事件就 MOD,否则 ADD;用 data.ptr 直接带上 FdContext
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    // 枚举当整数位掩码用:显式转 uint32_t(C++20 禁止不同枚举类型隐式位运算)
    epevent.events = EPOLLET | (uint32_t)fd_ctx->events | (uint32_t)event;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(epfd_, op, fd, &epevent);
    if(rt){
        BRONX_LOG_ERROR(g_logger) << "epoll_ctl(" << epfd_ << ", "
                                  << op << ", " << fd << ", " << (EPOLL_EVENTS)epevent.events << "):"
                                  << rt << " (" << errno << ") (" << strerror(errno) << ") fd_ctx->events="
                                  << (EPOLL_EVENTS)fd_ctx->events;
        return -1;
    }

    ++pendingEventCount_;

    // 记下 owner,close() 才能把 abortAll 路由到正确的 manager
    if(auto fdctx = FdMgr::GetInstance()->get(fd)){
        fdctx->setIOManager(this);
    }

    fd_ctx->events = (Event)(fd_ctx->events | event);
    FdContext::EventContext& event_ctx = fd_ctx->slotOf(event);
    BRONX_ASSERT(!event_ctx.scheduler && !event_ctx.cb && !event_ctx.fiber);

    // cb 为空就把当前协程挂上,事件就绪时恢复它
    event_ctx.scheduler = BxScheduler::Current();
    if(cb){
        event_ctx.cb.swap(cb);
    } else {
        event_ctx.fiber = BxFiber::Current();
        BRONX_ASSERT2(event_ctx.fiber->getState() == BxFiber::ACTIVE, "state=" << event_ctx.fiber->getState());
    }
    return 0;
}

// 删事件,不触发回调
bool BxIoManager::dropEvent(int fd, Event event){
    if(fd < 0){
        return false;
    }
    RWMutexType::ReadLock lockR(mutex_);
    if((int)fdContexts_.size() <= fd){
        return false;
    }
    FdContext* fd_ctx = fdContexts_[fd];
    lockR.unlock();

    FdContext::MutexType::Lock lock(fd_ctx->mutex);
    if(!(fd_ctx->events & event)){
        return false;
    }

    // 摘掉这个事件,没剩事件就把 fd 从 epoll 删掉
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epEvent;
    epEvent.events = EPOLLET | (uint32_t)new_events;
    epEvent.data.ptr = fd_ctx;

    int rt = epoll_ctl(epfd_, op, fd, &epEvent);
    if(rt){
        BRONX_LOG_ERROR(g_logger) << "epoll_ctl(" << epfd_ << ", "
                                  << op << ", " << fd << ", " << (EPOLL_EVENTS)epEvent.events << "):"
                                  << rt << " (" << errno << ") (" << strerror(errno) << ") fd_ctx->events="
                                  << (EPOLL_EVENTS)fd_ctx->events;
        return false;
    }

    --pendingEventCount_;
    fd_ctx->events = new_events;
    ClearFdOwnerIfIdle(fd, fd_ctx->events);
    FdContext::EventContext& event_ctx = fd_ctx->slotOf(event);
    fd_ctx->clearSlot(event_ctx);
    return true;
}

// 取消某事件,并强制触发一次它的回调(用来唤醒挂在上面的协程)
bool BxIoManager::abortEvent(int fd, Event event){
    if(fd < 0){
        return false;
    }
    RWMutexType::ReadLock lockR(mutex_);
    if((int)fdContexts_.size() <= fd){
        return false;
    }
    FdContext* fd_ctx = fdContexts_[fd];
    lockR.unlock();

    FdContext::MutexType::Lock lock(fd_ctx->mutex);
    if(!(fd_ctx->events & event)){
        return false;
    }

    // 摘掉这个事件,剩下没事件了就把 fd 从 epoll 删掉
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epEvent;
    epEvent.events = EPOLLET | (uint32_t)new_events;
    epEvent.data.ptr = fd_ctx;

    int rt = epoll_ctl(epfd_, op, fd, &epEvent);
    if(rt){
        BRONX_LOG_ERROR(g_logger) << "epoll_ctl(" << epfd_ << ", "
                                  << op << ", " << fd << ", " << (EPOLL_EVENTS)epEvent.events << "):"
                                  << rt << " (" << errno << ") (" << strerror(errno) << ") fd_ctx->events="
                                  << (EPOLL_EVENTS)fd_ctx->events;
        return false;
    }

    // 触发前先 bump 取消代次:被唤醒的 do_io 据此知道这次是"被取消"而非"IO 就绪",
    // 于是返回 ECANCELED 退出,而不是重试再挂起。
    if(auto ctx = FdMgr::GetInstance()->get(fd)){
        ctx->bumpCancelGen(event == EV_OUT);
    }
    fd_ctx->fireEvent(event);
    ClearFdOwnerIfIdle(fd, fd_ctx->events);
    --pendingEventCount_;
    return true;
}

bool BxIoManager::abortAll(int fd){
    if(fd < 0){
        return false;
    }
    RWMutexType::ReadLock lockR(mutex_);
    if((int)fdContexts_.size() <= fd){
        return false;
    }
    FdContext* fd_ctx = fdContexts_[fd];
    lockR.unlock();

    FdContext::MutexType::Lock lock(fd_ctx->mutex);
    if(!fd_ctx->events){
        return false;
    }

    // 全取消,直接把 fd 从 epoll 删掉
    int op = EPOLL_CTL_DEL;
    epoll_event epEvent;
    epEvent.events = 0;
    epEvent.data.ptr = fd_ctx;

    int rt = epoll_ctl(epfd_, op, fd, &epEvent);
    if(rt){
        BRONX_LOG_ERROR(g_logger) << "epoll_ctl(" << epfd_ << ", "
                                  << op << ", " << fd << ", " << (EPOLL_EVENTS)epEvent.events << "):"
                                  << rt << " (" << errno << ") (" << strerror(errno) << ") fd_ctx->events="
                                  << (EPOLL_EVENTS)fd_ctx->events;
        return false;
    }

    // 逐个补触发,同样先 bump 取消代次让 do_io 退出而非重试
    auto cancel_ctx = FdMgr::GetInstance()->get(fd);
    if(fd_ctx->events & EV_IN){
        if(cancel_ctx) cancel_ctx->bumpCancelGen(false);
        fd_ctx->fireEvent(EV_IN);
        --pendingEventCount_;
    }
    if(fd_ctx->events & EV_OUT){
        if(cancel_ctx) cancel_ctx->bumpCancelGen(true);
        fd_ctx->fireEvent(EV_OUT);
        --pendingEventCount_;
    }

    BRONX_ASSERT(fd_ctx->events == 0);
    ClearFdOwnerIfIdle(fd, fd_ctx->events);
    return true;
}

// 返回当前IO调度器
BxIoManager* BxIoManager::Current(){
    return dynamic_cast<BxIoManager*>(BxScheduler::Current());
}

BxIoManager::FdContext::EventContext& BxIoManager::FdContext::slotOf(Event event){
    switch(event){
    case BxIoManager::EV_IN:
        return readCtx;
    case BxIoManager::EV_OUT:
        return writeCtx;
    default:
        BRONX_ASSERT2(false, "slotOf");
    }
    throw std::invalid_argument("slotOf invaild event");
}

// 重置事件上下文
void BxIoManager::FdContext::clearSlot(EventContext& ctx){
    ctx.scheduler = nullptr;
    ctx.cb = nullptr;
    ctx.fiber.reset();
}

// 触发事件
void BxIoManager::FdContext::fireEvent(Event event){
    // 待触发的事件必须被注册过
    BRONX_ASSERT(events & event);
    // 触发是一次性的，这里删除被触发的事件
    events = (Event)(events & ~event);
    
    // 一次性:把回调/协程丢回调度器,然后清空这个事件槽
    EventContext& ctx = slotOf(event);
    if(ctx.cb){
        ctx.scheduler->post(ctx.cb);
    } else {
        ctx.scheduler->post(ctx.fiber);
    }
    clearSlot(ctx);
    return;
}

void BxIoManager::wakeup(){
    // 往管道写一字节戳醒 epoll_wait。就算此刻看不到 idle 线程也照写:idle 计数和
    // 进入 epoll_wait 之间有竞态,漏写会让刚阻塞的线程只能等超时才醒,拖慢 stop/cancel。
    int rt = 0;
    do{
        rt = write(pipefds_[1], "T", 1);
    } while(rt == -1 && errno == EINTR);
    if(rt == -1 && errno == EAGAIN){
        return;
    }
    BRONX_ASSERT(rt == 1);
}

bool BxIoManager::stopped(){
    // 无在途 IO + 无一次性定时器才可停。循环定时器(心跳/清理等常驻任务)不算,
    // 否则挂了循环定时器 stop() 会永远等不到。
    return BxScheduler::stopped() && (pendingEventCount_ == 0)
            && !hasPendingOneShot();
}

void BxIoManager::idle(){
    const uint64_t MAX_EVENTS = 256;    // 单次 epoll_wait 最多取这么多,多的下轮再取
    epoll_event* events = new epoll_event[MAX_EVENTS]();
    std::shared_ptr<epoll_event> shared_events(events, [](epoll_event* ptr){
        delete[] ptr;
    });

    while(true){
        if(stopped()){
            break;
        }

        int events_count = 0;
        do{
            static const int MAX_TIMEOUT = 5000;
            // 用下个定时器的剩余时间当超时,但封顶 5s。无定时器时 nextTimeout() 返回 ~0ull,
            // 若直接转 int 会变 -1 让 epoll_wait 无限阻塞 —— 一旦 stop() 的 wakeup 早于本线程
            // 进入 epoll_wait,它就永远醒不来复查 stopped(),多线程退出会死锁。所以退化成 5s。
            uint64_t next_raw = nextTimeout();
            int next_timer;
            if(next_raw == ~0ull){
                next_timer = MAX_TIMEOUT;
            } else {
                next_timer = (int)std::min(next_raw, (uint64_t)MAX_TIMEOUT);
            }

            events_count = epoll_wait(epfd_, events, MAX_EVENTS, next_timer);
            if(events_count < 0 && errno == EINTR){
                continue;   // 被信号打断,重试
            } else {
                break;
            }
        } while(true);

        // 先把到期定时器任务丢进队列
        std::vector<std::function<void()>> cbs;
        collectExpired(cbs);
        if(!cbs.empty()){
            post(cbs.begin(), cbs.end());
            cbs.clear();
        }

        for(int i = 0; i < events_count; ++i){
            epoll_event& event = events[i];

            // wakeup 管道:内容无意义,ET 模式下读干净即可
            if(event.data.fd == pipefds_[0]){
                uint8_t dummy[256];
                while(read(pipefds_[0], dummy, sizeof(dummy)) > 0);
                continue;
            }

            // 常规 IO:data.ptr 就是当初存的 FdContext
            FdContext* fd_ctx = (FdContext*)event.data.ptr;
            FdContext::MutexType::Lock lock(fd_ctx->mutex);
            // 出错/挂断当成可读可写处理,好让挂着的协程醒来收到错误
            if(event.events & (EPOLLERR | EPOLLHUP)){
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }
            int real_event = EV_NONE;
            if(event.events & EPOLLIN){
                real_event |= EV_IN;
            }
            if(event.events & EPOLLOUT){
                real_event |= EV_OUT;
            }
            if((real_event & fd_ctx->events) == EV_NONE){
                continue;
            }

            // 摘掉本次处理的事件,剩下的重新挂回 epoll
            int left_event = (fd_ctx->events & ~real_event);
            int op = left_event ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events = EPOLLET | left_event;

            int rt2 = epoll_ctl(epfd_, op, fd_ctx->fd, &event);
            if(rt2){
                BRONX_LOG_ERROR(g_logger) << "epoll_ctl(" << epfd_ << ", "
                                            << op << ", " << fd_ctx->fd << ", " << (EPOLL_EVENTS)event.events << "):"
                                            << rt2 << " (" << errno << ") (" << strerror(errno) << ") fd_ctx->events="
                                            << (EPOLL_EVENTS)fd_ctx->events;
                continue;
            }

            if(real_event & EV_IN){
                fd_ctx->fireEvent(EV_IN);
                --pendingEventCount_;
            }
            if(real_event & EV_OUT){
                fd_ctx->fireEvent(EV_OUT);
                --pendingEventCount_;
            }
            ClearFdOwnerIfIdle(fd_ctx->fd, fd_ctx->events);
        }

        // 这轮事件都丢回队列了,yield 回调度协程去 resume 那些协程,下次 resume 再从头 epoll
        BxFiber::ptr cur = BxFiber::Current();
        auto raw_ptr = cur.get();
        cur.reset();
        raw_ptr->yield();
    }
}

void BxIoManager::onEarliestChanged(){
    wakeup();
}


}
