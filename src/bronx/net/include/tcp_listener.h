#pragma once

#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <unordered_map>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "net_socket.h"
#include "reactor.h"
#include "endpoint.h"
#include "sync.h"
#include "noncopyable.h"


namespace bronx{

// BxTcpServer 运行状态
struct TcpServerStats {
    size_t   activeConnections = 0;   // 当前活跃连接数
    uint64_t acceptedTotal     = 0;   // 累计 accept 成功数
    uint64_t closedTotal       = 0;   // 累计连接关闭(onConnection 返回)数
    uint64_t acceptErrors      = 0;   // 累计 accept 失败数
    uint64_t rejectedTotal     = 0;   // 因超过 maxConnections 被拒绝的连接数
    bool     stopping          = false;
    bool     draining          = false;
};

// BxTcpServer 资源/超时选项
struct TcpServerOptions {
    size_t   maxConnections      = 10000;   // 最大并发连接数,超过则 accept 后立即关闭
    uint64_t drainTimeoutMs      = 30000;   // drain 后强制关闭剩余连接的超时
    uint64_t acceptErrorBackoffMs = 10;     // accept 连续失败时的退避(防 EMFILE 空转刷屏)
};

class BxTcpServer: public std::enable_shared_from_this<BxTcpServer>, public Noncopyable {
public:
    using ptr = std::shared_ptr<BxTcpServer>;

    BxTcpServer(BxIoManager* ioworker = BxIoManager::Current(),
              BxIoManager* acceptWorker = BxIoManager::Current());
    virtual ~BxTcpServer();

    // 绑定地址，返回绑定是否成功
    virtual bool bind(BxAddress::ptr addr);

    // 绑定一组地址，只有全部bind成功才返回true
    // fail: 存放绑定失败的地址
    virtual bool bind(const std::vector<BxAddress::ptr>& addrs, std::vector<BxAddress::ptr>& fails);

    // 启动服务(得先 bind 成功),把每个监听 socket 的 acceptLoop 投到 acceptWorker。
    // 已经在跑就直接返回 true(幂等)。主链起点。
    virtual bool start();

    // 停止服务:停止监听 + 关闭监听 socket + 关闭所有活跃连接
    virtual void stop();

    // 优雅排干:停接新连接 + 置 draining 标志(上层据此停 keep-alive)+ 超时后强关仍活着的连接。
    // timeout_ms=0 就用 options.drainTimeoutMs。"让在途请求做完再关"要 HTTP 层查 isDraining() 配合。
    virtual void drain(uint64_t timeout_ms = 0);

    bool waitDrain(uint64_t timeout_ms = 0);

    // 是否正在排干
    bool isDraining() const { return draining_.load(); }

    // 当前活跃连接数
    size_t getActiveConnectionCount() const { return activeConn_.load(); }

    // 运行状态快照
    TcpServerStats getStats() const;

    // 选项读写
    const TcpServerOptions& getOptions() const { return options_; }
    void setOptions(const TcpServerOptions& v) { options_ = v; }

    // 获取/设置超时时间
    uint64_t getRecvTimeout() const { return recvTimeout_; }
    void setRecvTimeout(uint64_t v) { recvTimeout_ = v; }

    // 获取/设置服务器名称
    std::string getName() const { return name_; }
    void setName(const std::string& v) { name_ = v; }

    // 是否停止
    bool isStop() const { return isStop_; }

    // 获取监听Socket
    std::vector<BxSocket::ptr> getSocks() const;

protected:
    // accept 循环,跑在 acceptWorker 的协程里:accept 出客户端 → 限流/draining 检查
    // → 登记连接表+计数 → 把 serveConnection 投到 ioworker。accept 失败按 errno 退避或退出。
    virtual void acceptLoop(BxSocket::ptr sock);

    // 处理单条客户端连接,业务入口,子类重写(基类只打印一下)。
    // 一般是拿 BxSocketStream 包一层 client 再收发,函数返回就代表这条连接处理完了。
    virtual void onConnection(BxSocket::ptr client);

    // 连接表操作(并发安全)
    uint64_t nextConnectionId() { return nextConnId_.fetch_add(1); }
    void addConnection(uint64_t id, BxSocket::ptr conn);
    void removeConnection(uint64_t id);
    // 关闭所有活跃连接(经各自 owner BxIoManager 路由 abortAll+close)
    void closeAllConnections();

private:
    // onConnection 的包装:执行业务 + 结束后从连接表注销 + 计数维护
    void serveConnection(BxSocket::ptr client, uint64_t id);

private:
    std::vector<BxSocket::ptr> takeListenSockets();
    void closeListenSockets();

private:
    // 监听Socket数组
    std::vector<BxSocket::ptr> socks_;
    mutable std::mutex socksMutex_;
    // 负责对新连接的Socket进行处理的调度器（调度器可作为线程池使用）
    BxIoManager* ioworker_;
    // 负责Socket接收连接的调度器（调度start与stop相关操作）
    BxIoManager* acceptWorker_;
    // 接收超时时间（ms），连接超时或客户端长时间未通信后断开连接
    uint64_t recvTimeout_;
    // 服务器名称
    std::string name_;

    // 服务器是否停止。atomic:stop() 与 acceptLoop(不同线程)并发读写。
    std::atomic<bool> isStop_;

    // --- 连接追踪(阶段3) ---
    // 连接表:id -> 活跃客户端连接(weak_ptr,不延长生命周期)。BxRwMutex 保护。
    mutable BxRwMutex connMutex_;
    std::unordered_map<uint64_t, std::weak_ptr<BxSocket>> connections_;
    std::atomic<uint64_t> nextConnId_{1};
    std::atomic<size_t>   activeConn_{0};
    std::atomic<bool>     draining_{false};
    std::mutex            drainMutex_;
    std::condition_variable drainCv_;
    BxTimer::ptr          drainTimer_;
    // 统计计数
    std::atomic<uint64_t> acceptedTotal_{0};
    std::atomic<uint64_t> closedTotal_{0};
    std::atomic<uint64_t> acceptErrors_{0};
    std::atomic<uint64_t> rejectedTotal_{0};
    // 选项
    TcpServerOptions options_;

protected:
    // 服务器类型
    std::string type_ = "tcp";
};


}
