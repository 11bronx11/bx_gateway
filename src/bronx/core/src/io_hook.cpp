#include "io_hook.h"
#include "log.h"
#include "config.h"
#include "fiber.h"
#include "reactor.h"
#include "fd_context.h"
#include <dlfcn.h>


static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

// connect 超时,默认 5s,可热更
static bronx::BxConfigVar<int>::ptr g_tcp_connect_timeout =
    bronx::BxConfig::Lookup("tcp.connect.timeout", 5000, "tcp connect timeout");

// 用一张表统一列出所有要 hook 的函数
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(setsockopt)



namespace bronx{

static thread_local bool t_hook_enable = false;

bool is_hook_enable(){
    return t_hook_enable;
}

void set_hook_enable(bool flag){
    t_hook_enable = flag;
}

void hook_init(){
    static bool is_inited = false;
    if(is_inited){
        return;
    }

// XX(sleep) 展开成 sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"),
// 即拿到 libc 里原始的 sleep(绕过我们这层 hook)
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
    HOOK_FUN(XX);
#undef XX
}

static uint64_t s_connect_timeout = -1;

// 静态对象在 main 之前构造,借此在程序启动时就完成 hook 初始化
struct _HookIniter
{
    _HookIniter(){
        hook_init();

        s_connect_timeout = g_tcp_connect_timeout->getValue();
        // 配置热更时同步
        g_tcp_connect_timeout->addListener([](const int& oldVal, const int& newVal){
            BRONX_LOG_INFO(g_logger) << "g_tcp_connect_timeout: " << oldVal << " to " << newVal;
            s_connect_timeout = newVal;
        });
    }
};
static _HookIniter s_hook_initer;

}


// 配合条件定时器:cancelled 记录这次等待是被超时取消(置 ETIMEDOUT)
struct timer_info{
    int cancelled = 0;
};


// 所有读写类 hook 的公共骨架。
// fun=原系统调用, event=READ/WRITE, timeout_so=SO_RCVTIMEO/SO_SNDTIMEO, args=除 fd 外的参数。
template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name,
        uint32_t event, int timeout_so, Args&&... args){
    if(!bronx::t_hook_enable){
        return fun(fd, std::forward<Args>(args)...);
    }

    bronx::BxFdCtx::ptr ctx = bronx::FdMgr::GetInstance()->get(fd);
    if(!ctx){
        return fun(fd, std::forward<Args>(args)...);   // 没上下文,当普通 fd
    }

    if(ctx->isClose()){
        errno = EBADF;
        return -1;
    }

    // 不归 hook 管、或用户自己设了非阻塞,都走原调用
    if(!ctx->isHookNonblock() || ctx->getUserNonblock()){
        return fun(fd, std::forward<Args>(args)...);
    }

    uint64_t to = ctx->getTimeout(timeout_so);
    std::shared_ptr<timer_info> tinfo(new timer_info);   // 条件定时器的存活条件

    // 核心循环:遇 EAGAIN 就挂事件 + yield,被唤醒后重试;设了超时还挂个定时器兜底。
    ssize_t n;
    while(true){
        do{
            n = fun(fd, std::forward<Args>(args)...);
        }while(n == -1 && errno == EINTR);   // 被信号打断,重试

        if(n == -1 && errno == EAGAIN){
            bronx::BxIoManager* iom = bronx::BxIoManager::Current();
            bronx::BxTimer::ptr timer;
            std::weak_ptr<timer_info> winfo(tinfo);

            // 关键:必须在 armEvent 之前采样取消代次。否则 armEvent 到采样之间若有别的线程
            // cancel 并 bump 代次,本协程采到的就是新值,resume 后比对相等 → 误判 IO 就绪去重试
            // → 纯 cancel 场景会重新挂事件永久卡住。提前采样才能覆盖 yield 期间的取消。
            bool is_write = (event == bronx::BxIoManager::EV_OUT);
            uint64_t cancel_gen = ctx->getCancelGen(is_write);

            if(to != (uint64_t)-1){
                // 超时定时器:到点了取消这个事件
                timer = iom->addConditionTimer(to, [winfo, fd, iom, event](){
                    auto t = winfo.lock();
                    if(!t || t->cancelled){
                        return;
                    }
                    // 先尝试取消,只有真取消成功(事件还挂着、确由超时唤醒)才标 ETIMEDOUT。
                    // 否则:IO 其实已就绪、事件位已被 idle 清掉,abortEvent 返回 false,
                    // 这时不能乱标超时,不然会把已就绪的 IO 误判成超时丢掉。
                    if(iom->abortEvent(fd, (bronx::BxIoManager::Event)(event))){
                        t->cancelled = ETIMEDOUT;
                    }
                }, winfo);
            }

            // 添加事件监听
            // addEvent参数中的回调函数为空，表示将本协程作为回调，IO事件触发后会resume到现在这个协程
            int rt = iom->armEvent(fd, (bronx::BxIoManager::Event)(event));
            if(rt){
                // addEvent调用失败，记录错误日志，并取消定时器，防止误触发
                BRONX_LOG_ERROR(g_logger) << hook_fun_name << ": armEvent("
                        << fd << ", " << event <<")";
                if(timer){
                    timer->cancel();
                }
                return -1;
            } else {
                // (取消代次 cancel_gen 已在进入 EAGAIN 分支时采样,见上方 C1 修复)

                // 注册事件成功，本协程yield让出执行权，等待注册的IO事件发生后再resume
                bronx::BxFiber::Current()->yield();

                // 注册的I/O事件触发或定时器超时，resume回到这里
                if(timer){
                    // 同上，取消定时器防止误触发
                    timer->cancel();
                }

                // resume的原因是定时器超时，则返回-1表示操作失败
                if(tinfo->cancelled == ETIMEDOUT){
                    errno = tinfo->cancelled;
                    return -1;
                }

                // 代次变了 = 这次唤醒来自取消,直接 ECANCELED 退出,
                // 不能重试(重试还会 EAGAIN 再挂事件,就永久卡住了)。
                if(ctx->getCancelGen(is_write) != cancel_gen){
                    errno = ECANCELED;
                    return -1;
                }

                // 否则是 IO 就绪,回到循环开头重试
                continue;
            }
        }
        break;
    }
    return n;
}


extern "C"{

// 定义那批 *_f 指针,初值 nullptr(hook_init 里填上)
#define XX(name) name ## _fun name ## _f = nullptr;
    HOOK_FUN(XX)
#undef XX

// sleep 类:挂个定时器,到点把本协程 post 回来,中间 yield 让出——异步 sleep
unsigned int sleep(unsigned int seconds){
    if(!bronx::is_hook_enable()){
        return sleep_f(seconds);
    }

    bronx::BxFiber::ptr fiber = bronx::BxFiber::Current();
    bronx::BxIoManager* iom = bronx::BxIoManager::Current();
    iom->addTimer(seconds * 1000, [fiber, iom](){
        iom->post(fiber, -1);
    });
    fiber->yield();
    return 0;
}

int usleep(useconds_t usec){
    if(!bronx::is_hook_enable()){
        return usleep_f(usec);
    }

    bronx::BxFiber::ptr fiber = bronx::BxFiber::Current();
    bronx::BxIoManager* iom = bronx::BxIoManager::Current();
    iom->addTimer(usec / 1000, [fiber, iom](){   // 定时器精度到毫秒
        iom->post(fiber, -1);
    });
    fiber->yield();
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem){
    if(!bronx::is_hook_enable()){
        return nanosleep_f(req, rem);
    }
    int time_ms = req->tv_sec * 1000 + req->tv_nsec / 1000 / 1000;
    bronx::BxFiber::ptr fiber = bronx::BxFiber::Current();
    bronx::BxIoManager* iom = bronx::BxIoManager::Current();
    iom->addTimer(time_ms, [fiber, iom](){
        iom->post(fiber, -1);
    });
    fiber->yield();
    return 0;
}

int socket(int domain, int type, int protocol){
    if(!bronx::is_hook_enable()){
        return socket_f(domain, type, protocol);
    }

    int fd = socket_f(domain, type, protocol);
    if(fd >= 0){
        bronx::FdMgr::GetInstance()->get(fd, true);   // 纳入 FdManager
    }
    return fd;
}

// 带超时的 connect,思路跟 do_io 类似
int connect_with_timeout(int sockfd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms){
    if(!bronx::is_hook_enable()){
        return connect_f(sockfd, addr, addrlen);
    }
    bronx::BxFdCtx::ptr ctx = bronx::FdMgr::GetInstance()->get(sockfd);
    if(!ctx || ctx->isClose()){
        errno = EBADF;
        return -1;
    }

    if(!ctx->isSocket()){
        return connect_f(sockfd, addr, addrlen);
    }
    if(ctx->getUserNonblock()){
        return connect_f(sockfd, addr, addrlen);
    }

    int n = connect_f(sockfd, addr, addrlen);
    if(n == 0){
        return 0;
    } else if(n != -1 || errno != EINPROGRESS){
        // 不是"连接进行中",直接返回结果
        return n;
    }

    // EINPROGRESS:连接还在进行,跟 do_io 处理 EAGAIN 一样,挂事件 + 定时器等它完成
    bronx::BxIoManager* iom = bronx::BxIoManager::Current();
    bronx::BxTimer::ptr timer;
    std::shared_ptr<timer_info> tinfo(new timer_info);
    std::weak_ptr<timer_info> winfo(tinfo);

    // 添加定时器
    if(timeout_ms != (uint64_t)-1){
        timer = iom->addConditionTimer(timeout_ms, [winfo, sockfd, iom](){
            auto t = winfo.lock();
            if(!t || t->cancelled){
                return;
            }
            // 同 do_io:只有真取消成功才标超时,否则会把已就绪的 connect 误判成超时
            if(iom->abortEvent(sockfd, bronx::BxIoManager::EV_OUT)){
                t->cancelled = ETIMEDOUT;
            }
        }, winfo);
    }

    // 挂事件前先采样代次,好在等待期间被 abortAll/close 时识别出来
    uint64_t cancel_gen = ctx->getCancelGen(/*write=*/true);

    // 连接完成会触发可写事件
    int rt = iom->armEvent(sockfd, bronx::BxIoManager::EV_OUT);
    if(rt == 0){
        bronx::BxFiber::Current()->yield();
        // 被唤醒
        if(timer){
            timer->cancel();
        }
        if(tinfo->cancelled == ETIMEDOUT){
            errno = tinfo->cancelled;
            return -1;
        }
        // 代次变了说明 fd 正被取消/关闭,别再 getsockopt(可能已被并发 close),按取消处理
        if(ctx->getCancelGen(/*write=*/true) != cancel_gen){
            errno = ECANCELED;
            return -1;
        }
    } else {
        BRONX_LOG_ERROR(g_logger) << "connect armEvent(" << sockfd << ", WRITE)";
        if(timer){
            timer->cancel();
        }
        return -1;
    }

    // 用 SO_ERROR 确认连接到底成没成
    int error = 0;
    socklen_t len = sizeof(int);
    if(getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) == -1){
        return -1;
    }
    if(!error){
        return 0;
    } else {
        errno = error;
        return -1;
    }
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen){
    return connect_with_timeout(sockfd, addr, addrlen, bronx::s_connect_timeout);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen){
    // 新连接到来 = 监听 socket 可读
    bool hook_enabled = bronx::is_hook_enable();
    int fd = do_io(sockfd, accept_f, "accept", bronx::BxIoManager::EV_IN, SO_RCVTIMEO, addr, addrlen);
    // 只有 hook 线程才接管新 fd。普通线程保持 libc 语义,不然会拿到意外的 EAGAIN、
    // 关闭时还容易留下陈旧的 BxFdCtx。
    if(fd >= 0 && hook_enabled){
        bronx::FdMgr::GetInstance()->get(fd, true);
    }
    return fd;
}

ssize_t read(int fd, void *buf, size_t count){
    return do_io(fd, read_f, "read", bronx::BxIoManager::EV_IN, SO_RCVTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt){
    return do_io(fd, readv_f, "readv", bronx::BxIoManager::EV_IN, SO_RCVTIMEO, iov, iovcnt);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags){
    return do_io(sockfd, recv_f, "recv", bronx::BxIoManager::EV_IN, SO_RCVTIMEO, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen){
    return do_io(sockfd, recvfrom_f, "recvfrom", bronx::BxIoManager::EV_IN, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags){
    return do_io(sockfd, recvmsg_f, "recvmsg", bronx::BxIoManager::EV_IN, SO_RCVTIMEO, msg, flags);
}

// write
ssize_t write(int fd, const void *buf, size_t count){
    return do_io(fd, write_f, "write", bronx::BxIoManager::EV_OUT, SO_SNDTIMEO, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt){
    return do_io(fd, writev_f, "writev", bronx::BxIoManager::EV_OUT, SO_SNDTIMEO, iov, iovcnt);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags){
    return do_io(sockfd, send_f, "send", bronx::BxIoManager::EV_OUT, SO_SNDTIMEO, buf, len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen){
    return do_io(sockfd, sendto_f, "sendto", bronx::BxIoManager::EV_OUT, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags){
    return do_io(sockfd, sendmsg_f, "sendmsg", bronx::BxIoManager::EV_OUT, SO_SNDTIMEO, msg, flags);
}


int close(int fd){
    // 关闭前清理 BxFdCtx:这个 fd 可能是在 hook 线程建、后来交给普通线程关的,
    // 直接 close_f 会留下陈旧 BxFdCtx,fd 号复用后新 socket 会被误当成已初始化,搞坏 epoll/hook。
    bronx::BxFdCtx::ptr ctx = bronx::FdMgr::GetInstance()->get(fd);
    if(ctx){
        // abortAll 要路由到真正持有该 fd 事件的 owner(armEvent 时登记的),
        // 不能用当前线程的 Current()——可能为空或是别的 manager,导致协程唤不醒、epoll 残留。
        bronx::BxIoManager* iom = ctx->getIOManager();
        if(!iom){
            iom = bronx::BxIoManager::Current();   // 没登记过就退回当前 manager
        }
        if(iom){
            iom->abortAll(fd);
        }
        bronx::FdMgr::GetInstance()->del(fd);
    }
    return close_f(fd);
}


// fcntl:只在 F_SETFL/F_GETFL 上做手脚(维护用户非阻塞标志),其余透传
int fcntl(int fd, int cmd, ... /* arg */ ){
    va_list va;
    va_start(va, cmd);
    switch(cmd){
        case F_SETFL:
            {
                int arg = va_arg(va, int);
                va_end(va);

                bronx::BxFdCtx::ptr ctx = bronx::FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClose() || !ctx->isSocket()){
                    return fcntl_f(fd, cmd, arg);
                }
                // 记住用户想要的非阻塞;实际是否非阻塞由系统标志决定(socket 总是非阻塞)
                ctx->setUserNonblock(arg & O_NONBLOCK);
                if(ctx->getSysNonblock()){
                    arg |= O_NONBLOCK;
                } else {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_f(fd, cmd, arg);
            }
            break;

        case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd);
                bronx::BxFdCtx::ptr ctx = bronx::FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClose() || !ctx->isSocket()){
                    return arg;
                }

                // 对用户呈现他自己设的非阻塞状态,而非系统实际的
                if(ctx->getUserNonblock()){
                    return arg | O_NONBLOCK;
                } else {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;

    // 其余类型1：输入参数为int
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
        {
            int arg = va_arg(va, int);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }
        break;

    // 其余类型2：输入参数为void
        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
        {
            va_end(va);
            return fcntl_f(fd, cmd);
        }
        break;

    // 其余类型3：输入参数为flock*
        case F_SETLK:
        case F_SETLKW:
        case F_GETLK:
        case F_OFD_GETLK:
        case F_OFD_SETLK:
        case F_OFD_SETLKW:
        {
            struct flock* arg = va_arg(va, struct flock*);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }
        break;

    // 其余类型4：输入参数为f_owner_ex*
        case F_GETOWN_EX:
        case F_SETOWN_EX:
        {
            struct f_owner_ex* arg = va_arg(va, struct f_owner_ex*);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }
        break;
    // 默认
        default:
        {
            va_end(va);
            return fcntl_f(fd, cmd);
        }
    }
}

int ioctl(int fd, unsigned long request, ...){
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);

    // FIONBIO 是另一种设非阻塞的方式,同样只记用户标志,真正的非阻塞交给系统层
    if(request == FIONBIO){
        if(!arg){
            return ioctl_f(fd, request, arg);
        }
        bool usrNonblock = !!*(int*)arg;
        bronx::BxFdCtx::ptr ctx = bronx::FdMgr::GetInstance()->get(fd);
        if(!ctx || ctx->isClose() || !ctx->isSocket()){
            return ioctl_f(fd, request, arg);
        }
        ctx->setUserNonblock(usrNonblock);
        if(ctx->getSysNonblock()){
            int sysNonblock = 1;
            return ioctl_f(fd, request, &sysNonblock);
        }
    }
    return ioctl_f(fd, request, arg);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen){
    if(!bronx::is_hook_enable()){
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }

    // 拦下收发超时,记进 BxFdCtx,do_io 要用它当 addConditionTimer 的超时
    if(level == SOL_SOCKET){
        if(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO){
            bronx::BxFdCtx::ptr ctx = bronx::FdMgr::GetInstance()->get(sockfd);
            if(ctx){
                const timeval* v = (const timeval*)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    return setsockopt_f(sockfd, level, optname, optval, optlen);
}



}
