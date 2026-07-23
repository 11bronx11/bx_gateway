#include "net_socket.h"
#include "log.h"
#include "fd_context.h"
#include "macro.h"
#include "io_hook.h"
#include "reactor.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <climits>


static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

namespace bronx{

static void SetUnixAddressLen(const BxAddress::ptr& addr, socklen_t addrlen){
    if(addr && addr->getFamily() == AF_UNIX){
        BxUnixAddress::ptr unix_addr = std::dynamic_pointer_cast<BxUnixAddress>(addr);
        if(unix_addr){
            unix_addr->setAddrLen(addrlen);
        }
    }
}

static BxIoManager* GetFdIOManager(int fd){
    BxFdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
    if(ctx && ctx->getIOManager()){
        return ctx->getIOManager();
    }
    return BxIoManager::Current();
}

static bool WaitConnectReady(int sock, uint64_t timeout_ms){
    int timeout = -1;
    if(timeout_ms != (uint64_t)-1){
        timeout = timeout_ms > (uint64_t)INT_MAX ? INT_MAX : (int)timeout_ms;
    }

    pollfd pfd;
    pfd.fd = sock;
    pfd.events = POLLOUT;
    pfd.revents = 0;

    int rt = 0;
    do {
        rt = ::poll(&pfd, 1, timeout);
    } while(rt < 0 && errno == EINTR);

    if(rt == 0){
        errno = ETIMEDOUT;
        return false;
    }
    if(rt < 0){
        return false;
    }

    int error = 0;
    socklen_t len = sizeof(error);
    if(::getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len)){
        return false;
    }
    if(error){
        errno = error;
        return false;
    }
    return true;
}

// 创建TCP BxSocket
BxSocket::ptr BxSocket::MakeTcp(BxAddress::ptr addr){
    if(!addr){
        return nullptr;
    }
    BxSocket::ptr sock(new BxSocket(addr->getFamily(), TCP, 0));
    return sock;
}

// 创建UDP BxSocket
BxSocket::ptr BxSocket::MakeUdp(BxAddress::ptr addr){
    if(!addr){
        return nullptr;
    }
    BxSocket::ptr sock(new BxSocket(addr->getFamily(), UDP, 0));
    sock->newSock();
    // UDP是无连接的
    sock->isConnected_ = sock->isValid();
    return sock;
}

// 创建IPv4的TCP BxSocket
BxSocket::ptr BxSocket::MakeTcpSocket(){
    BxSocket::ptr sock(new BxSocket(IPv4, TCP, 0));
    return sock;
}

// 创建IPv6的TCP BxSocket
BxSocket::ptr BxSocket::MakeTcpSocket6(){
    BxSocket::ptr sock(new BxSocket(IPv6, TCP, 0));
    return sock;   
}

// 创建Unix的TCP BxSocket
BxSocket::ptr BxSocket::MakeUnixTcpSocket(){
    BxSocket::ptr sock(new BxSocket(Unix, TCP, 0));
    return sock;
}

// 创建IPv4的UDP BxSocket
BxSocket::ptr BxSocket::MakeUdpSocket(){
    BxSocket::ptr sock(new BxSocket(IPv4, UDP, 0));
    sock->newSock();
    sock->isConnected_ = sock->isValid();
    return sock;
}

// 创建IPv6的UDP BxSocket
BxSocket::ptr BxSocket::MakeUdpSocket6(){
    BxSocket::ptr sock(new BxSocket(IPv6, UDP, 0));
    sock->newSock();
    sock->isConnected_ = sock->isValid();
    return sock;
}

// 创建Unix的UDP BxSocket
BxSocket::ptr BxSocket::MakeUnixUdpSocket(){
    BxSocket::ptr sock(new BxSocket(Unix, UDP, 0));
    sock->newSock();
    sock->isConnected_ = sock->isValid();
    return sock;
}


// 构造函数
BxSocket::BxSocket(int family, int type, int protocol)
    : sock_(-1)
    , family_(family)
    , type_(type)
    , protocol_(protocol)
    , isConnected_(false){   
}

// 析构函数
BxSocket::~BxSocket(){
    close();
}

// 获取发送超时时间
int64_t BxSocket::getSendTimeout() const{
    BxFdCtx::ptr ctx = FdMgr::GetInstance()->get(sock_);
    if(ctx){
        return ctx->getTimeout(SO_SNDTIMEO);
    }
    return -1;
}

// 设置发送超时时间
void BxSocket::setSendTimeout(int64_t v){
    struct timeval tv{ int(v / 1000), int(v % 1000 * 1000) };
    if(setOption(SOL_SOCKET, SO_SNDTIMEO, tv)){
        BxFdCtx::ptr ctx = FdMgr::GetInstance()->get(sock_);
        if(ctx){
            ctx->setTimeout(SO_SNDTIMEO, v);
        }
    }
}

// 获取接收超时时间
int64_t BxSocket::getRecvTimeout() const{
    BxFdCtx::ptr ctx = FdMgr::GetInstance()->get(sock_);
    if(ctx){
        return ctx->getTimeout(SO_RCVTIMEO);
    }
    return -1;    
}

// 设置接收超时时间
void BxSocket::setRecvTimeout(int64_t v){
    struct timeval tv{ int(v / 1000), int(v % 1000 * 1000) };
    if(setOption(SOL_SOCKET, SO_RCVTIMEO, tv)){
        BxFdCtx::ptr ctx = FdMgr::GetInstance()->get(sock_);
        if(ctx){
            ctx->setTimeout(SO_RCVTIMEO, v);
        }
    }
}

// 获取sockopt
bool BxSocket::getOption(int level, int optname, void *result, socklen_t *optlen){
    int rt = getsockopt(sock_, level, optname, result, (socklen_t*)optlen);
    if(rt){
        BRONX_LOG_ERROR(g_logger) << "getOption sock=" << sock_ 
                                  << " level=" << level << "option=" << optname
                                  << " errno=" << errno << " errstr=" << strerror(errno);
        return false;
    }
    return true;
}

// 设置sockopt
bool BxSocket::setOption(int level, int optname, const void *optval, socklen_t optlen){
    int rt = setsockopt(sock_, level, optname, optval, optlen);
    if(rt){
        BRONX_LOG_DEBUG(g_logger) << "setOption sock=" << sock_ 
                                  << " level=" << level << " option=" << optname
                                  << " errno=" << errno << " errstr=" << strerror(errno);
        return false;
    }
    return true;
}

// accept 一个新连接,造出客户端 BxSocket。没连接时经 hook 协程让出。
// 成功返回 init 好的客户端 socket,失败 nullptr(看 errno 区分原因)。
// BxTcpServer::acceptLoop 的 accept 协程里循环调它,产物投给 onConnection。
BxSocket::ptr BxSocket::accept(){
    // sock用于存放连接发起方的socket（即远端socket）
    BxSocket::ptr sock(new BxSocket(family_, type_, protocol_));
    int newsock = ::accept(sock_, nullptr, nullptr);
    if(newsock == -1){
        if(errno == ECANCELED || errno == EBADF) {
            return nullptr;
        }
        BRONX_LOG_ERROR(g_logger) << "accept(" << sock_ << ") errno="
                                  << errno << " errstr=" << strerror(errno);
        return nullptr;
    }
    // 调用init初始化sock
    if(sock->init(newsock)){
        return sock;
    }
    return nullptr;
}

// 封装bind，将一个地址绑定到socket上
bool BxSocket::bind(const BxAddress::ptr addr){
    if(!addr){
        BRONX_LOG_ERROR(g_logger) << "bind addr is null";
        return false;
    }
    // 检查socket
    if(!isValid()){
        // 如果当前socket无效，就创建一个新的socket
        newSock();
        if(BRONX_UNLIKELY(!isValid())){
            return false;
        }
    }
    // 检查family
    if(BRONX_UNLIKELY(addr->getFamily() != family_)){
        BRONX_LOG_ERROR(g_logger) << "bind sock.family("
                                  << family_ << ") addr.family(" << addr->getFamily()
                                  << ") not equal, addr=" << addr->toString();
        return false;
    }

    // 调用bind
    if(::bind(sock_, addr->getAddr(), addr->getAddrLen())){
        BRONX_LOG_ERROR(g_logger) << "bind error errrno=" << errno
                                  << " errstr=" << strerror(errno);
        return false;
    }
    // bind 之后这个 socket 表示本地监听/收包端，不应继续暴露旧连接的远端地址。
    remoteAddress_.reset();
    // 不要预先把请求地址记成 localAddress_：当请求端口为 0（内核自动分配）时，
    // 这会让 getLocalAddress() 永远返回端口 0。清空缓存，让 getLocalAddress()
    // 走 getsockname 拿到真实绑定地址（含内核分配的端口）。
    localAddress_.reset();
    getLocalAddress();
    return true;
}

// connect 到远端(客户端方向,非服务端 accept 主链)
// 输入:addr 远端地址(family 校验后缓存供 reconnect);timeout_ms=-1 不限时。
// 输出:成功 true 并填本地/远端地址;失败 false 并 close 自身。经 hook 协程让出。
bool BxSocket::connect(const BxAddress::ptr addr, uint64_t timeout_ms){
    if(!addr){
        BRONX_LOG_ERROR(g_logger) << "connect addr is null";
        return false;
    }
    // 与bind类似，封装connect操作：检查socket - 调用connect
    // 检查socket
    if(!isValid()){
        // 如果当前socket无效，就创建一个新的socket
        newSock();
        if(BRONX_UNLIKELY(!isValid())){
            return false;
        }
    }
    // 检查family
    if(BRONX_UNLIKELY(addr->getFamily() != family_)){
        BRONX_LOG_ERROR(g_logger) << "connect sock.family("
                                  << family_ << ") addr.family(" << addr->getFamily()
                                  << ") not equal, addr=" << addr->toString();
        return false;
    }

    // addr 是远端地址。family 校验通过后再缓存，避免失败的错误族地址污染 reconnect。
    // 即使本次 connect 因 Connection refused 失败，也保留 remote 供后续 reconnect 重试。
    remoteAddress_ = addr;
    if(timeout_ms == (uint64_t)-1){
        // 没有设置超时时间，直接调用connect
        if(::connect(sock_, addr->getAddr(), addr->getAddrLen())){
            if(errno == EINPROGRESS && WaitConnectReady(sock_, timeout_ms)){
                isConnected_ = true;
                getLocalAddress();
                getRemoteAddress();
                return true;
            }
            BRONX_LOG_ERROR(g_logger) << "sock=" << sock_ << " connect(" << addr->toString()
                                      << ") error errno=" << errno << " errstr=" << strerror(errno);
            // 调用失败时需关闭socket
            close();
            return false;
        }
    } else {
        // 设置了超时时间，调用connect_with_timeout
        if(::connect_with_timeout(sock_, addr->getAddr(), addr->getAddrLen(), timeout_ms)){
            if(errno == EINPROGRESS && WaitConnectReady(sock_, timeout_ms)){
                isConnected_ = true;
                getLocalAddress();
                getRemoteAddress();
                return true;
            }
            BRONX_LOG_ERROR(g_logger) << "sock=" << sock_ << " connect(" << addr->toString()
                                      << ") timeout=" << timeout_ms << " error errno="
                                      << errno << " errstr=" << strerror(errno);
            close();
            return false;
        }
    }
    isConnected_ = true;
    // 更新connect的远端地址和本地地址（本地地址由 getsockname 拉真实临时端口）
    getLocalAddress();
    getRemoteAddress();
    return true;
}

// 重新连接远端地址
bool BxSocket::reconnect(uint64_t timeout_ms){
    if(!remoteAddress_){
        BRONX_LOG_ERROR(g_logger) << "reconnect: remote address is null";
        return false;
    }
    localAddress_.reset();
    return connect(remoteAddress_, timeout_ms);
}

// 封装listen，监听socket
bool BxSocket::listen(int backlog){
    if(!isValid()){
        BRONX_LOG_ERROR(g_logger) << "listen error sock=-1";
        return false;
    }
    if(::listen(sock_, backlog)){
        BRONX_LOG_ERROR(g_logger) << "listen error errno=" << errno
                                  << " errstr=" << strerror(errno);
        return false;
    }
    return true;
}

// 关闭 socket(幂等)。经 owner BxIoManager 路由 abortAll 唤醒阻塞协程(带 ECANCELED 返回),
// 再注销 FdMgr 并 ::close。这是 BxTcpServer::stop/drain 能踢掉活跃连接的底座。总是返回 true。
bool BxSocket::close(){
    if(!isConnected_ && sock_ == -1){
        // socket未连接/未创建，无需关闭，视为成功
        return true;
    }
    isConnected_ = false;
    // 调用close
    if(sock_ != -1){
        int sock = sock_;
        // [C4 修复] 路由 abortAll 到持有该 fd 事件的 owner BxIoManager,而非当前线程的
        // BxIoManager::Current()(跨线程/多 BxIoManager 时会发错地方,协程不被唤醒)。
        BxFdCtx::ptr ctx = FdMgr::GetInstance()->get(sock);
        BxIoManager* iom = ctx ? ctx->getIOManager() : nullptr;
        if(!iom){
            iom = BxIoManager::Current();
        }
        if(iom){
            iom->abortAll(sock);
        }
        FdMgr::GetInstance()->del(sock);
        ::close(sock);
        sock_ = -1;
    }
    // 关闭 fd 后本地地址会失效；保留 remote 地址以支持 reconnect()。
    localAddress_.reset();
    // 成功关闭应返回 true（原实现成功时返回 false，语义反了；
    // 当前无调用方依赖返回值，修正为 true 不影响其它模块）。
    return true;
}

// 发送数据(经 hook:发送缓冲满时协程让出)。>0 已发字节数;<0 错误(未连接 -1)。
// 被 BxSocketStream::write 调用。
int BxSocket::send(const void *buf, size_t len, int flags){
    if(isConnected()){
        return ::send(sock_, buf, len, flags);
    }
    return -1;
}

int BxSocket::send(const iovec *buf, size_t len, int flags){
    // sendmsg
    if(isConnected()){
        msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = (iovec*)buf;
        msg.msg_iovlen = len;
        return ::sendmsg(sock_, &msg, flags);
    }
    return -1;
}

int BxSocket::sendTo(const void *buf, size_t len, const BxAddress::ptr toAddr, int flags){
    if(isConnected() && toAddr){
        return ::sendto(sock_, buf, len, flags, toAddr->getAddr(), toAddr->getAddrLen());
    }
    return -1;   
}

int BxSocket::sendTo(const iovec *buf, size_t len, const BxAddress::ptr toAddr, int flags){
    // sendmsg
    if(isConnected() && toAddr){
        msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = (iovec*)buf;
        msg.msg_iovlen = len;
        // to地址写入msg
        msg.msg_name = toAddr->getAddr();
        msg.msg_namelen = toAddr->getAddrLen();
        return ::sendmsg(sock_, &msg, flags);
    }
    return -1; 
}

// 接收数据(经 hook:无数据时协程让出)。>0 字节数;=0 对端关闭;<0 错误(未连接 -1)。
// 被 BxSocketStream::read 调用,是服务端收数据的最底层落点。
int BxSocket::recv(void *buf, size_t len, int flags){
    if(isConnected()){
        return ::recv(sock_, buf, len, flags);
    }
    return -1;    
}

int BxSocket::recv(iovec *buf, size_t len, int flags){
    // recvmsg
    if(isConnected()){
        msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = (iovec*)buf;
        msg.msg_iovlen = len;
        return ::recvmsg(sock_, &msg, flags);
    }
    return -1;   
}

int BxSocket::recvfrom(void *buf, size_t len, BxAddress::ptr fromAddr, int flags){
    if(isConnected() && fromAddr){
        // 注意：接收长度用入参 len（buffer 长度），地址长度单独用 addrlen，
        // 不能让局部变量遮蔽 len，否则会把地址长度当成 buffer 长度。
        socklen_t addrlen = fromAddr->getAddrLen();
        int rt = ::recvfrom(sock_, buf, len, flags, fromAddr->getAddr(), &addrlen);
        if(rt >= 0){
            SetUnixAddressLen(fromAddr, addrlen);
        }
        return rt;
    }
    return -1;
}

int BxSocket::recvfrom(iovec *buf, size_t len, BxAddress::ptr fromAddr, int flags){
    // recvmsg
    if(isConnected() && fromAddr){
        msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = (iovec*)buf;
        msg.msg_iovlen = len;
        msg.msg_name = fromAddr->getAddr();
        msg.msg_namelen = fromAddr->getAddrLen();
        int rt = ::recvmsg(sock_, &msg, flags);
        if(rt >= 0){
            SetUnixAddressLen(fromAddr, msg.msg_namelen);
        }
        return rt;
    }
    return -1;  
}

// 获取连接的远端地址
BxAddress::ptr BxSocket::getRemoteAddress(){
    if(remoteAddress_){
        // 已经有了远端地址
        return remoteAddress_;
    }

    // 没有远端地址，需要创建一个
    BxAddress::ptr result;
    switch(family_){
        case AF_INET:
            result.reset(new BxIpv4Address());
            break;
        case AF_INET6:
            result.reset(new BxIpv6Address());
            break;
        case AF_UNIX:
            result.reset(new BxUnixAddress());
            break;
        default:
            result.reset(new BxUnknownAddress(family_));
            break;
    }

    // 调用getpeername获取远端地址
    socklen_t addrlen = result->getAddrLen();
    if(getpeername(sock_, result->getAddr(), &addrlen)){
        BRONX_LOG_ERROR(g_logger) << "getpeername error sock=" << sock_
                                  << " errno=" << errno << " errstr=" << strerror(errno);
        return BxAddress::ptr(new BxUnknownAddress(family_));
    }
    if(family_ == AF_UNIX){
        // Unix地址需要单独设置地址长度
        BxUnixAddress::ptr addr = std::dynamic_pointer_cast<BxUnixAddress>(result);
        addr->setAddrLen(addrlen);
    }
    remoteAddress_ = result;
    return remoteAddress_;
}

// 获取连接的本地地址
BxAddress::ptr BxSocket::getLocalAddress(){
    // 同getRemoteAddress，但调用getsockname获取本地地址
    if(localAddress_){
        return localAddress_;
    }

    // 没有本地地址，需要创建一个
    BxAddress::ptr result;
    switch(family_){
        case AF_INET:
            result.reset(new BxIpv4Address());
            break;
        case AF_INET6:
            result.reset(new BxIpv6Address());
            break;
        case AF_UNIX:
            result.reset(new BxUnixAddress());
            break;
        default:
            result.reset(new BxUnknownAddress(family_));
            break;
    }

    // 调用getsockname获取本地地址
    socklen_t addrlen = result->getAddrLen();
    if(getsockname(sock_, result->getAddr(), &addrlen)){
        BRONX_LOG_ERROR(g_logger) << "getsockname error sock=" << sock_
                                  << " errno=" << errno << " errstr=" << strerror(errno);
        return BxAddress::ptr(new BxUnknownAddress(family_));
    }
    if(family_ == AF_UNIX){
        // Unix地址需要单独设置地址长度
        BxUnixAddress::ptr addr = std::dynamic_pointer_cast<BxUnixAddress>(result);
        addr->setAddrLen(addrlen);
    }
    localAddress_ = result;
    return localAddress_;
}

// 返回当前socket是否有效
bool BxSocket::isValid() const{
    return (sock_ != -1);
}

// 返回socket错误
int BxSocket::getError(){
    int error = 0;
    socklen_t len = sizeof(error);
    // 通过getsockopt获取error
    if(!getOption(SOL_SOCKET, SO_ERROR, &error, &len)){
        error = errno;
    }
    return error;
}

// 输出信息到流中
std::ostream& BxSocket::dump(std::ostream& os) const {
    os << "[BxSocket sock=" << sock_
       << " is_connected=" << isConnected_
       << " family=" << family_
       << " type=" << type_
       << " protocol=" << protocol_;
    if(localAddress_){
        os << " local_address=" << localAddress_->toString();
    }
    if(remoteAddress_){
        os << " remote_address=" << remoteAddress_->toString();
    }
    os << "]";
    return os;
}

std::string BxSocket::toString() const {
    std::stringstream ss;
    dump(ss);
    return ss.str();
}

// 取消读
bool BxSocket::abortRead(){
    if(sock_ == -1){
        return false;
    }
    BxIoManager* iom = GetFdIOManager(sock_);
    return iom ? iom->abortEvent(sock_, BxIoManager::EV_IN) : false;
}

// 取消写
bool BxSocket::abortWrite(){
    if(sock_ == -1){
        return false;
    }
    BxIoManager* iom = GetFdIOManager(sock_);
    return iom ? iom->abortEvent(sock_, BxIoManager::EV_OUT) : false;
}

// 取消accept
bool BxSocket::abortAccept(){
    if(sock_ == -1){
        return false;
    }
    BxIoManager* iom = GetFdIOManager(sock_);
    return iom ? iom->abortEvent(sock_, BxIoManager::EV_IN) : false;
}

// 取消该 fd 上所有事件:经 owner BxIoManager 触发,被取消的协程带 ECANCELED 返回。
// 被 close() 及 BxTcpServer 关连接路径间接使用。
bool BxSocket::abortAll(){
    if(sock_ == -1){
        return false;
    }
    BxIoManager* iom = GetFdIOManager(sock_);
    return iom ? iom->abortAll(sock_) : false;
}

// 初始化socket
void BxSocket::initSock(){
    int val = 1;
    setOption(SOL_SOCKET, SO_REUSEADDR, val);
    if(type_ == SOCK_STREAM && (family_ == AF_INET || family_ == AF_INET6)){
        // 针对TCP需要初始化额外内容
        // 如果不设置TCP_NODELY，TCP会延迟发送（期间缓存一组数据后一次性发送），这里选择不延迟
        setOption(IPPROTO_TCP, TCP_NODELAY, val);
    }
}

// 创建socket
void BxSocket::newSock(){
    // 封装socket
    sock_ = socket(family_, type_, protocol_);
    if(BRONX_UNLIKELY(sock_ == -1)){
        BRONX_LOG_ERROR(g_logger) << "socket(" << family_
            << ", " << type_ << ", " << protocol_ << ") errno="
            << errno << " errstr=" << strerror(errno);
    } else {
        FdMgr::GetInstance()->get(sock_, true);
        initSock();
    }
}

// 接收一个int类型的sockfd来初始化自身
bool BxSocket::init(int sock){
    BxFdCtx::ptr ctx = FdMgr::GetInstance()->get(sock, true);
    if(ctx && ctx->isSocket() && !ctx->isClose()){
        sock_ = sock;
        isConnected_ = true;
        initSock();
        getLocalAddress();
        getRemoteAddress();
        return true;
    }
    return false;
}


// 重载Socket opeartor<<
std::ostream& operator<<(std::ostream& os, const BxSocket& sock){
    return sock.dump(os);
}

}
