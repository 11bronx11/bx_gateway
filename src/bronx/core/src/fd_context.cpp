#include "fd_context.h"
#include "io_hook.h"
#include <sys/stat.h>


namespace bronx{

BxFdCtx::BxFdCtx(int fd)
    : isInit_(false)
    , isSocket_(false)
    , sysNonblock_(false)
    , usrNonblock_(false)
    , isClosed_(false)
    , hookNonblock_(false)
    , fd_(fd)
    , recvTimeout_(-1)
    , sendTimeout_(-1){
    init();
}

BxFdCtx::~BxFdCtx(){
}


void BxFdCtx::setTimeout(int type, uint64_t v){
    if(type == SO_RCVTIMEO){
        recvTimeout_ = v;
    } else {
        sendTimeout_ = v;
    }
}

uint64_t BxFdCtx::getTimeout(int type){
    if(type == SO_RCVTIMEO){
        return recvTimeout_;
    } else if(type == SO_SNDTIMEO) {
        return sendTimeout_;
    } else {
        return -1;
    }
}


bool BxFdCtx::init(){
    if(isInit_){
        return true;
    }

    // fstat 探测是不是 socket(失败就保持默认 false)
    struct stat fd_stat;
    if(fstat(fd_, &fd_stat) != -1){
        isInit_ = true;
        isSocket_ = S_ISSOCK(fd_stat.st_mode);
    }

    // socket 强制设非阻塞以配合 epoll ET
    if(isSocket_){
        int flags = fcntl_f(fd_, F_GETFL, 0);
        if(!(flags & O_NONBLOCK)){
            fcntl_f(fd_, F_SETFL, flags | O_NONBLOCK);
        }
        sysNonblock_ = true;
        hookNonblock_ = true;
    }

    return isInit_;
}


BxFdManager::BxFdManager(){
    fds_.resize(64);
}

BxFdCtx::ptr BxFdManager::get(int fd, bool auto_create){
    if(fd < 0){
        return nullptr;
    }

    {
        RWMutexType::ReadLock lockR(mutex_);
        // 已存在,或不存在但也不要求创建 —— 读锁下就能返回
        if(fd < (int)fds_.size() && (fds_[fd] || !auto_create)){
            return fds_[fd];
        }
        if(fd >= (int)fds_.size() && !auto_create){
            return nullptr;
        }
    }

    // 要创建,升写锁
    RWMutexType::WriteLock lockW(mutex_);
    if(fd >= (int)fds_.size()){
        // resize 必须在写锁下,且新容量严格大于 fd
        fds_.resize(fd * 3 / 2 + 1);
    }
    if(fds_[fd]){
        return fds_[fd];
    }
    BxFdCtx::ptr ctx(new BxFdCtx(fd));
    fds_[fd] = ctx;
    return ctx;
}

void BxFdManager::del(int fd){
    if(fd < 0){
        return;
    }
    RWMutexType::WriteLock lockW(mutex_);
    if((int)fds_.size() <= fd){
        return;
    }
    // reset 而非 erase,槽位留着复用
    fds_[fd].reset();
}




}
