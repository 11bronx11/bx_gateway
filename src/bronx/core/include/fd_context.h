#pragma once

#include "sync.h"
#include "singleton.h"
#include <vector>
#include <memory>
#include <atomic>


namespace bronx{

class BxIoManager;  // BxFdCtx 要记住是哪个 manager 持有该 fd 的事件

// 一个 fd 的运行期属性集合:是不是 socket、非阻塞标志、读写超时、是否关闭、
// 读写取消代次、以及事件 owner。hook / BxSocket / BxIoManager 共用这份"fd 真相"。
class BxFdCtx{
public:
    using ptr = std::shared_ptr<BxFdCtx>;

    BxFdCtx(int fd);

    ~BxFdCtx();

    bool isInit() const { return isInit_; }

    bool isSocket() const { return isSocket_; }

    // 是否交给 hook/do_io 按非阻塞 fd 处理。socket 默认开启；
    // offload(eventfd) 这类非 socket fd 可在创建后显式开启。
    bool isHookNonblock() const { return hookNonblock_; }
    void setHookNonblock(bool v){ hookNonblock_ = v; }

    bool isClose() const { return isClosed_; }

    void setUserNonblock(bool v){ usrNonblock_ = v; }
    bool getUserNonblock() const { return usrNonblock_; }
    void setSysNonblock(bool v){ sysNonblock_ = v; }
    bool getSysNonblock() const { return sysNonblock_; }

    // type: SO_RCVTIMEO(读) / SO_SNDTIMEO(写)
    void setTimeout(int type, uint64_t v);
    uint64_t getTimeout(int type);

    // 取消代次:abortEvent/abortAll 会 bump 对应方向的代次。do_io 在 yield 前后比对,
    // 变了就说明这次唤醒是"被取消"而非 IO 就绪,于是返回 ECANCELED 退出、不再重挂事件。
    uint64_t getCancelGen(bool write) const {
        return write ? writeCancelGen_.load(std::memory_order_acquire)
                     : readCancelGen_.load(std::memory_order_acquire);
    }
    void bumpCancelGen(bool write){
        if(write) writeCancelGen_.fetch_add(1, std::memory_order_release);
        else      readCancelGen_.fetch_add(1, std::memory_order_release);
    }

    // 记住持有该 fd 事件的 manager。close() 据此把 abortAll 路由到正确的 manager,
    // 而不是当前线程的 Current()(可能为空或是别的 manager,导致协程唤不醒、epoll 残留)。
    BxIoManager* getIOManager() const { return iomanager_.load(std::memory_order_acquire); }
    void setIOManager(BxIoManager* v){ iomanager_.store(v, std::memory_order_release); }

private:
    bool init();

private:
    // 位域省空间
    bool isInit_: 1;
    bool isSocket_: 1;
    bool sysNonblock_: 1;      // 系统层非阻塞(hook 设的)
    bool usrNonblock_: 1;      // 用户显式设的非阻塞
    bool isClosed_: 1;
    bool hookNonblock_: 1;     // 是否允许 hook 对该 fd 做协程化等待

    int fd_;
    uint64_t recvTimeout_;
    uint64_t sendTimeout_;
    std::atomic<uint64_t> readCancelGen_{0};
    std::atomic<uint64_t> writeCancelGen_{0};
    std::atomic<BxIoManager*> iomanager_{nullptr};
};


// 全进程 fd -> BxFdCtx 的管理器(单例 FdMgr)。fd 值直接作 vector 下标,读写锁保护。
// hook / BxSocket / BxIoManager 都从这里查 fd 元信息。
class BxFdManager{
public:
    using RWMutexType = BxRwMutex;

    BxFdManager();

    // 取 fd 上下文;auto_create 且不存在时新建并 init(探测类型、设非阻塞)。
    BxFdCtx::ptr get(int fd, bool auto_create = false);
    void del(int fd);

private:
    RWMutexType mutex_;
    std::vector<BxFdCtx::ptr> fds_;
};

using FdMgr = Singleton<BxFdManager>;

}
