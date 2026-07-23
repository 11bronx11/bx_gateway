#include "tcp_listener.h"
#include "log.h"
#include "config.h"
#include <chrono>
#include <unistd.h>


static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");


// 配置项：服务器超时时间，默认2min
static bronx::BxConfigVar<uint64_t>::ptr g_tcp_server_recv_timeout =
    bronx::BxConfig::Lookup("tcp_server.recv_timeout", (uint64_t)(60 * 1000 * 2),
            "tcp server recv timeout");


namespace bronx{


// 构造函数
BxTcpServer::BxTcpServer(BxIoManager* ioworker, BxIoManager* acceptWorker)
    : ioworker_(ioworker)
    , acceptWorker_(acceptWorker)
    , recvTimeout_(g_tcp_server_recv_timeout->getValue())
    , name_("bronx/1.0.0")
    , isStop_(true){
}

// 析构函数
BxTcpServer::~BxTcpServer(){
    closeListenSockets();
}

// 绑定地址，返回绑定是否成功
bool BxTcpServer::bind(BxAddress::ptr addr){
    // 创建一个TCP socket
    BxSocket::ptr sock = BxSocket::MakeTcp(addr);

    // 将地址绑定到创建的TCP socket上
    if(!sock->bind(addr)){
        BRONX_LOG_ERROR(g_logger) << "bind fail errno="
            << errno << " errstr=" << strerror(errno)
            << " addr=[" << addr->toString() << "]";
        return false;
    }

    // 对bind完的socket创建监听
    if(!sock->listen()){
        BRONX_LOG_ERROR(g_logger) << "listen fail errno="
            << errno << " errstr=" << strerror(errno)
            << " addr=[" << addr->toString() << "]";
        return false;
    }

    // bind成功加入m_socks，并日志socket信息
    {
        std::lock_guard<std::mutex> lock(socksMutex_);
        socks_.push_back(sock);
    }
    BRONX_LOG_INFO(g_logger) << "type=" << type_
        << " name=" << name_
        << " server bind success: " << *sock;

    return true;
}

// 绑定一组地址，只有全部bind成功才返回true
// fail: 存放绑定失败的地址
bool BxTcpServer::bind(const std::vector<BxAddress::ptr>& addrs, std::vector<BxAddress::ptr>& fails){
    for(auto& addr : addrs){
        if(!bind(addr)){
            fails.push_back(addr);
        }
    }

    if(!fails.empty()){
        closeListenSockets();
        // 只有全部bind成功才返回true
        return false;
    }

    return true;
}

// 启动服务（需要bind成功后才能执行）
bool BxTcpServer::start(){
    if(!isStop_){
        return true;
    }

    isStop_ = false;
    draining_ = false;
    std::vector<BxSocket::ptr> socks;
    {
        std::lock_guard<std::mutex> lock(socksMutex_);
        socks = socks_;
    }
    // 启动所有Socket
    for(auto& sock : socks){
        // 将startAccept作为被调度的协程，在协程中进行accept操作
        acceptWorker_->post(std::bind(&BxTcpServer::acceptLoop,
                    shared_from_this(), sock));
    }
    return true;
}

// <!-- PLACEHOLDER -->

// 停止服务:停止监听 + 关闭监听 socket + 关闭所有活跃连接
void BxTcpServer::stop(){
    isStop_ = true;
    // 捕获自身防止析构竞争
    auto self = shared_from_this();
    acceptWorker_->post([self](){
        self->closeListenSockets();
    });
    // 关闭所有已 accept 的活跃连接(偿还"stop 踢不掉长连接"的架构债)
    closeAllConnections();
}

// 优雅排干:停止接新 + 立即踢空闲连接 + 超时兜底强关
void BxTcpServer::drain(uint64_t timeout_ms){
    if(draining_.exchange(true)){
        return; // 已在排干
    }
    if(timeout_ms == 0){
        timeout_ms = options_.drainTimeoutMs;
    }
    BRONX_LOG_INFO(g_logger) << "BxTcpServer draining, timeout=" << timeout_ms
        << "ms active=" << activeConn_.load();

    // 1.停止接收新连接:关闭监听socket(accept协程会因 abortAll 醒来退出)
    auto self = shared_from_this();
    acceptWorker_->post([self](){
        self->closeListenSockets();
    });

    // 2.立即踢掉所有连接:空闲 keep-alive 阻塞在 recv 等下一请求,不踢就得等
    //   idleTimeoutMs(120s)才超时,远超典型 SIGTERM 等待窗口。
    //   对正在处理请求的连接,close 触发 ECANCELED,协程检测写失败后自然退出,
    //   activeConn_ 计数随即归零。
    closeAllConnections();

    if(activeConn_.load() == 0){
        return;
    }

    // 3.超时兜底:极端情况(协程卡住未退出)再强关一次
    auto timer = acceptWorker_->addTimer(timeout_ms, [this, self](){
        size_t remain = activeConn_.load();
        if(remain > 0){
            BRONX_LOG_INFO(g_logger) << "drain timeout, force closing "
                << remain << " connections";
            closeAllConnections();
        }
    }, false);

    bool cancel = false;
    {
        std::lock_guard<std::mutex> lock(drainMutex_);
        if(activeConn_.load() == 0){
            cancel = true;
        } else {
            drainTimer_ = timer;
        }
    }
    if(cancel){
        timer->cancel();
    }
}

std::vector<BxSocket::ptr> BxTcpServer::takeListenSockets(){
    std::lock_guard<std::mutex> lock(socksMutex_);
    std::vector<BxSocket::ptr> socks;
    socks.swap(socks_);
    return socks;
}

void BxTcpServer::closeListenSockets(){
    for(auto& sock : takeListenSockets()){
        sock->close();
    }
}

std::vector<BxSocket::ptr> BxTcpServer::getSocks() const {
    std::lock_guard<std::mutex> lock(socksMutex_);
    return socks_;
}

bool BxTcpServer::waitDrain(uint64_t timeout_ms){
    if(timeout_ms == 0){
        timeout_ms = options_.drainTimeoutMs;
    }
    auto done = [this](){
        return activeConn_.load() == 0;
    };
    std::unique_lock<std::mutex> lock(drainMutex_);
    if(drainCv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), done)){
        auto timer = std::move(drainTimer_);
        lock.unlock();
        if(timer) timer->cancel();
        return true;
    }
    lock.unlock();
    closeAllConnections();
    lock.lock();
    bool ok = drainCv_.wait_for(lock, std::chrono::milliseconds(500), done);
    auto timer = ok ? std::move(drainTimer_) : nullptr;
    lock.unlock();
    if(timer) timer->cancel();
    return ok;
}

// 连接表:登记
void BxTcpServer::addConnection(uint64_t id, BxSocket::ptr conn){
    BxRwMutex::WriteLock lock(connMutex_);
    connections_[id] = conn;   // 存 weak_ptr
}

// 连接表:注销
void BxTcpServer::removeConnection(uint64_t id){
    BxRwMutex::WriteLock lock(connMutex_);
    connections_.erase(id);
}

// 关闭所有活跃连接。BxSocket::close 内部经 owner BxIoManager 路由 abortAll(阶段2 C4),
// 唤醒阻塞在该连接上的 onConnection 协程,使其返回并自然注销。close 幂等,无 double-close。
void BxTcpServer::closeAllConnections(){
    std::vector<BxSocket::ptr> conns;
    {
        BxRwMutex::ReadLock lock(connMutex_);
        conns.reserve(connections_.size());
        for(auto& kv : connections_){
            if(auto s = kv.second.lock()){
                conns.push_back(s);
            }
        }
    }
    for(auto& s : conns){
        s->close();
    }
}

// 运行状态快照
TcpServerStats BxTcpServer::getStats() const {
    TcpServerStats st;
    st.activeConnections = activeConn_.load();
    st.acceptedTotal     = acceptedTotal_.load();
    st.closedTotal       = closedTotal_.load();
    st.acceptErrors      = acceptErrors_.load();
    st.rejectedTotal     = rejectedTotal_.load();
    st.stopping          = isStop_;
    st.draining          = draining_.load();
    return st;
}

// 开始接受连接（对listen中的socket进行accept操作）
void BxTcpServer::acceptLoop(BxSocket::ptr sock){
    // server未停止时持续接收连接
    while(!isStop_){
        // accept接收连接(走 hook do_io,无连接时协程让出)
        BxSocket::ptr client = sock->accept();
        if(client){
            acceptedTotal_.fetch_add(1);

            // 超过最大连接数:立即关闭新连接(不投入处理)
            if(activeConn_.load() >= options_.maxConnections){
                rejectedTotal_.fetch_add(1);
                BRONX_LOG_WARN(g_logger) << "max connections reached ("
                    << options_.maxConnections << "), rejecting client "
                    << *client;
                client->close();
                continue;
            }
            // draining 期间不再处理新连接
            if(draining_.load()){
                client->close();
                continue;
            }

            // 设置客户端socket的超时时间
            client->setRecvTimeout(recvTimeout_);
            // 登记连接表 + 计数
            uint64_t id = nextConnectionId();
            activeConn_.fetch_add(1);
            addConnection(id, client);
            // 将handleClientWrapper放入m_ioworker进行调度
            ioworker_->post(std::bind(&BxTcpServer::serveConnection,
                        shared_from_this(), client, id));
        } else {
            // accept失败:轻量退避,防止 EMFILE/ENFILE 等持续失败时空转刷屏
            acceptErrors_.fetch_add(1);
            if(isStop_ || draining_.load() || errno == ECANCELED || errno == EBADF){
                break;
            }
            if(!isStop_){
                BRONX_LOG_ERROR(g_logger) << "accept errno=" << errno
                    << " errstr=" << strerror(errno)
                    << " (backoff " << options_.acceptErrorBackoffMs << "ms)";
                if(options_.acceptErrorBackoffMs > 0){
                    // sleep 走 hook:协程让出,不阻塞线程
                    usleep(options_.acceptErrorBackoffMs * 1000);
                }
            }
        }
    }
}

// onConnection 包装:执行业务 + 结束后注销 + 计数维护
void BxTcpServer::serveConnection(BxSocket::ptr client, uint64_t id){
    onConnection(client);
    removeConnection(id);
    if(activeConn_.fetch_sub(1) == 1){
        BxTimer::ptr timer;
        {
            std::lock_guard<std::mutex> lock(drainMutex_);
            timer = std::move(drainTimer_);
        }
        if(timer) timer->cancel();
    }
    closedTotal_.fetch_add(1);
    drainCv_.notify_all();
}

// 处理新连接的Socket
void BxTcpServer::onConnection(BxSocket::ptr client){
    BRONX_LOG_INFO(g_logger) << "onConnection: " << *client;
}


}
