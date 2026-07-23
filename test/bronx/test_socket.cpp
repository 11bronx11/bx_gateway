// BxSocket 模块系统测试
// =====================
// 设计要点：
//  - 不依赖公网：全部走 127.0.0.1 / ::1 / unix 临时路径
//  - 不用固定端口：bind 到 :0 让内核分配，再用 getLocalAddress() 取真实端口
//  - 可重复运行、与顺序无关：每个用例自带 server/client、用完即清理
//  - 收发并发用 std::thread 驱动（非 scheduler 线程内 hook 不开，但 bronx 创建的
//    fd 已被 fd_manager 设为非阻塞，所以阻塞型 accept/recv 需配合 poll 等待）
//  - cancel 一组单独在 BxIoManager 协程里测
//
// 用例索引见 main() 末尾。

#include "net_socket.h"
#include "endpoint.h"
#include "reactor.h"
#include "log.h"
#include "io_hook.h"
#include "fd_context.h"
#include "sock_stream.h"
#include "bytearray.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();
static int g_failed = 0;
static int g_checks = 0;

#define EXPECT(expr)                                                          \
    do {                                                                      \
        ++g_checks;                                                           \
        if(!(expr)) {                                                         \
            BRONX_LOG_ERROR(g_logger) << "FAIL: " #expr                       \
                                      << " @ " << __FILE__ << ":" << __LINE__;\
            ++g_failed;                                                       \
        }                                                                     \
    } while(0)

// 等 fd 可读/可写（bronx fd 默认非阻塞），超时返回 false
static bool wait_fd(int fd, short ev, int timeout_ms){
    struct pollfd pfd{ fd, ev, 0 };
    int r = ::poll(&pfd, 1, timeout_ms);
    return r > 0 && (pfd.revents & ev);
}

// 阻塞式 accept 封装：先 poll listen fd 再 accept
static bronx::BxSocket::ptr accept_with_wait(bronx::BxSocket::ptr listen_sock, int timeout_ms){
    if(!wait_fd(listen_sock->getSocket(), POLLIN, timeout_ms)){
        return nullptr;
    }
    return listen_sock->accept();
}

// 阻塞式 recv 封装：先 poll 再 recv
static int recv_with_wait(bronx::BxSocket::ptr sock, void* buf, size_t len, int timeout_ms){
    if(!wait_fd(sock->getSocket(), POLLIN, timeout_ms)){
        return -2; // 超时
    }
    return sock->recv(buf, len);
}

// ========== 一、工厂函数 ==========
static void test_factory(){
    BRONX_LOG_INFO(g_logger) << "--- test_factory ---";

    // TCP IPv4：延迟创建 fd
    {
        auto s = bronx::BxSocket::MakeTcpSocket();
        EXPECT(s->getFamily() == AF_INET);
        EXPECT(s->getType() == bronx::BxSocket::TCP);
        EXPECT(s->getProtocal() == 0);
        EXPECT(s->getSocket() == -1);       // 延迟创建
        EXPECT(!s->isValid());
        EXPECT(!s->isConnected());
    }
    // TCP IPv6：延迟创建
    {
        auto s = bronx::BxSocket::MakeTcpSocket6();
        EXPECT(s->getFamily() == AF_INET6);
        EXPECT(s->getType() == bronx::BxSocket::TCP);
        EXPECT(s->getSocket() == -1);
        EXPECT(!s->isValid());
    }
    // Unix TCP：延迟创建
    {
        auto s = bronx::BxSocket::MakeUnixTcpSocket();
        EXPECT(s->getFamily() == AF_UNIX);
        EXPECT(s->getType() == bronx::BxSocket::TCP);
        EXPECT(s->getSocket() == -1);
    }
    // UDP IPv4：立即创建 fd，且 isConnected()==true（仅表示 fd 可用）
    {
        auto s = bronx::BxSocket::MakeUdpSocket();
        EXPECT(s->getFamily() == AF_INET);
        EXPECT(s->getType() == bronx::BxSocket::UDP);
        EXPECT(s->getSocket() != -1);
        EXPECT(s->isValid());
        EXPECT(s->isConnected());           // UDP 工厂里直接置 true
    }
    // UDP IPv6：立即创建
    {
        auto s = bronx::BxSocket::MakeUdpSocket6();
        EXPECT(s->getFamily() == AF_INET6);
        EXPECT(s->getType() == bronx::BxSocket::UDP);
        EXPECT(s->getSocket() != -1);
        EXPECT(s->isConnected());
    }
    // Unix UDP：与其它 UDP 工厂一致——立即 newSock + 置 isConnected
    {
        auto s = bronx::BxSocket::MakeUnixUdpSocket();
        EXPECT(s->getFamily() == AF_UNIX);
        EXPECT(s->getType() == bronx::BxSocket::UDP);
        EXPECT(s->getSocket() != -1);       // 已与 v4/v6 UDP 一致：立即创建
        EXPECT(s->isConnected());
    }
    // MakeTcp(addr) / MakeUdp(addr)：按 addr 的 family 创建
    {
        auto a4 = bronx::BxIpAddress::Create("127.0.0.1", 0);
        auto tcp = bronx::BxSocket::MakeTcp(a4);
        EXPECT(tcp->getFamily() == AF_INET);
        EXPECT(tcp->getType() == bronx::BxSocket::TCP);

        auto udp = bronx::BxSocket::MakeUdp(a4);
        EXPECT(udp->getFamily() == AF_INET);
        EXPECT(udp->getType() == bronx::BxSocket::UDP);
        EXPECT(udp->getSocket() != -1);     // MakeUdp 会 newSock
        EXPECT(udp->isConnected());
    }
    // 空地址工厂：不应崩溃
    {
        EXPECT(!bronx::BxSocket::MakeTcp(nullptr));
        EXPECT(!bronx::BxSocket::MakeUdp(nullptr));
    }
    // UDP 工厂底层 socket 创建失败时，不能错误标记为 connected
    {
        bronx::BxAddress::ptr bad_addr(new bronx::BxUnknownAddress(-1));
        auto bad_udp = bronx::BxSocket::MakeUdp(bad_addr);
        EXPECT(bad_udp);
        EXPECT(!bad_udp->isValid());
        EXPECT(!bad_udp->isConnected());
    }
    // BxSocket 自己创建 fd 时应同步创建 BxFdCtx；close 后应删除 BxFdCtx
    {
        auto udp = bronx::BxSocket::MakeUdpSocket();
        int fd = udp->getSocket();
        EXPECT(fd >= 0);
        EXPECT(bronx::FdMgr::GetInstance()->get(fd));
        EXPECT(udp->close());
        EXPECT(!bronx::FdMgr::GetInstance()->get(fd));
        EXPECT(!bronx::FdMgr::GetInstance()->get(-2));
        bronx::FdMgr::GetInstance()->del(-2);
    }
}


// 通用 TCP echo 往返（IPv4/IPv6 共用）
static void tcp_roundtrip(int family, const std::string& loopback){
    auto listen_addr = bronx::BxIpAddress::Create(loopback.c_str(), 0);
    EXPECT(listen_addr);
    if(!listen_addr) return;

    auto server = (family == AF_INET) ? bronx::BxSocket::MakeTcpSocket()
                                      : bronx::BxSocket::MakeTcpSocket6();
    EXPECT(server->bind(listen_addr));
    EXPECT(server->listen());

    auto bound = std::dynamic_pointer_cast<bronx::BxIpAddress>(server->getLocalAddress());
    EXPECT(bound);
    if(!bound) return;
    uint16_t port = bound->getPort();
    EXPECT(port != 0);

    std::atomic<bool> srv_ok{false};
    std::thread srv_th([&](){
        auto cli = accept_with_wait(server, 3000);
        EXPECT(cli);
        if(!cli) return;
        EXPECT(cli->isConnected());
        EXPECT(cli->getLocalAddress());
        EXPECT(cli->getRemoteAddress());
        auto la = std::dynamic_pointer_cast<bronx::BxIpAddress>(cli->getLocalAddress());
        EXPECT(la && la->getPort() == port);   // 服务端 local 端口 == 监听端口

        char buf[64] = {0};
        int n = recv_with_wait(cli, buf, sizeof(buf)-1, 3000);
        EXPECT(n > 0);
        EXPECT(std::string(buf, n>0?n:0) == "hello");
        EXPECT(cli->send("world", 5) == 5);
        srv_ok = true;
        cli->close();
        EXPECT(!cli->isValid());
    });

    auto client = (family == AF_INET) ? bronx::BxSocket::MakeTcpSocket()
                                      : bronx::BxSocket::MakeTcpSocket6();
    auto connect_addr = bronx::BxIpAddress::Create(loopback.c_str(), port);
    EXPECT(client->connect(connect_addr));
    EXPECT(client->isConnected());

    // 关键回归：connect 后 local != remote，remote 端口 == server 端口
    auto cl = std::dynamic_pointer_cast<bronx::BxIpAddress>(client->getLocalAddress());
    auto cr = std::dynamic_pointer_cast<bronx::BxIpAddress>(client->getRemoteAddress());
    EXPECT(cl && cr);
    if(cl && cr){
        EXPECT(cr->getPort() == port);          // remote 是 server 端口
        EXPECT(cl->getPort() != 0);             // local 是内核分配的临时端口
        EXPECT(cl->getPort() != cr->getPort()); // 暴露旧 connect/local bug
    }

    EXPECT(client->send("hello", 5) == 5);
    char buf[64] = {0};
    int n = recv_with_wait(client, buf, sizeof(buf)-1, 3000);
    EXPECT(n == 5);
    EXPECT(std::string(buf, n>0?n:0) == "world");

    srv_th.join();
    EXPECT(srv_ok.load());

    client->close();
    EXPECT(!client->isValid());
    EXPECT(client->send("x", 1) < 0);
    char tmp[4];
    EXPECT(client->recv(tmp, sizeof(tmp)) < 0);

    server->close();
}


// ========== 二、TCP IPv4 ==========
static void test_tcp_v4(){
    BRONX_LOG_INFO(g_logger) << "--- test_tcp_v4 ---";
    tcp_roundtrip(AF_INET, "127.0.0.1");
}


// ========== 三、TCP IPv6（不支持则跳过）==========
static bool ipv6_loopback_available(){
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if(fd < 0) return false;
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    ::inet_pton(AF_INET6, "::1", &a.sin6_addr);
    bool ok = (::bind(fd, (sockaddr*)&a, sizeof(a)) == 0);
    ::close(fd);
    return ok;
}

static void test_tcp_v6(){
    BRONX_LOG_INFO(g_logger) << "--- test_tcp_v6 ---";
    if(!ipv6_loopback_available()){
        BRONX_LOG_WARN(g_logger) << "IPv6 loopback unavailable, SKIP test_tcp_v6";
        return;
    }
    tcp_roundtrip(AF_INET6, "::1");
}

// ========== 四、UDP IPv4 + recvfrom 长度回归 ==========
static void test_udp_v4(){
    BRONX_LOG_INFO(g_logger) << "--- test_udp_v4 ---";

    auto srv = bronx::BxSocket::MakeUdpSocket();
    auto srv_bind = bronx::BxIpAddress::Create("127.0.0.1", 0);
    EXPECT(srv->bind(srv_bind));
    auto srv_addr = std::dynamic_pointer_cast<bronx::BxIpAddress>(srv->getLocalAddress());
    EXPECT(srv_addr && srv_addr->getPort() != 0);
    if(!srv_addr) return;

    // 关键回归：发送一个比 sockaddr_in（16字节）更长的 payload。
    // 旧 recvfrom 把地址长度当 buffer 长度，会把 >16 字节的数据截断成 16。
    const std::string payload = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"; // 36B > 16
    std::atomic<bool> srv_done{false};
    std::thread srv_th([&](){
        char buf[128] = {0};
        EXPECT(wait_fd(srv->getSocket(), POLLIN, 3000));
        auto from = bronx::BxIpAddress::Create("127.0.0.1", 0);
        int n = srv->recvfrom(buf, sizeof(buf)-1, from);
        EXPECT(n == (int)payload.size());        // 不能被截断成 16
        EXPECT(std::string(buf, n>0?n:0) == payload);
        EXPECT(from->getPort() != 0);            // 来源地址被填充
        // 回发
        EXPECT(srv->sendTo("ack", 3, from) == 3);
        srv_done = true;
    });

    auto cli = bronx::BxSocket::MakeUdpSocket();
    EXPECT(cli->sendTo(payload.data(), payload.size(), srv_addr) == (int)payload.size());

    char rbuf[16] = {0};
    EXPECT(wait_fd(cli->getSocket(), POLLIN, 3000));
    auto from2 = bronx::BxIpAddress::Create("127.0.0.1", 0);
    int rn = cli->recvfrom(rbuf, sizeof(rbuf)-1, from2);
    EXPECT(rn == 3);
    EXPECT(std::string(rbuf, rn>0?rn:0) == "ack");

    srv_th.join();
    EXPECT(srv_done.load());

    // 边界：空数据报（0 长度）应可发可收
    {
        std::thread th([&](){
            char b[8];
            EXPECT(wait_fd(srv->getSocket(), POLLIN, 2000));
            auto f = bronx::BxIpAddress::Create("127.0.0.1", 0);
            int n = srv->recvfrom(b, sizeof(b), f);
            EXPECT(n == 0);                       // 0 长度 UDP 报文
        });
        auto c2 = bronx::BxSocket::MakeUdpSocket();
        EXPECT(c2->sendTo("", 0, srv_addr) == 0);
        th.join();
        c2->close();
    }

    cli->close();
    srv->close();
}

#ifdef __linux__
// ========== 四 b、Unix UDP + abstract 来源地址长度回归 ==========
static void test_unix_udp(){
    BRONX_LOG_INFO(g_logger) << "--- test_unix_udp ---";

    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "%d", (int)getpid());
    std::string srv_name(1, '\0');
    srv_name += "bronx-udg-srv-";
    srv_name += suffix;
    std::string cli_name(1, '\0');
    cli_name += "bronx-udg-cli-";
    cli_name += suffix;

    auto srv_addr = bronx::BxUnixAddress::Create(srv_name);
    auto cli_addr = bronx::BxUnixAddress::Create(cli_name);
    EXPECT(srv_addr && cli_addr);
    if(!(srv_addr && cli_addr)) return;

    auto srv = bronx::BxSocket::MakeUnixUdpSocket();
    auto cli = bronx::BxSocket::MakeUnixUdpSocket();
    EXPECT(srv->bind(srv_addr));
    EXPECT(cli->bind(cli_addr));

    const std::string payload = "unix-dgram-payload";
    EXPECT(cli->sendTo(payload.data(), payload.size(), srv_addr) == (int)payload.size());
    EXPECT(wait_fd(srv->getSocket(), POLLIN, 3000));

    char buf[64] = {0};
    bronx::BxUnixAddress::ptr from(new bronx::BxUnixAddress);
    int n = srv->recvfrom(buf, sizeof(buf) - 1, from);
    EXPECT(n == (int)payload.size());
    EXPECT(std::string(buf, n > 0 ? n : 0) == payload);
    EXPECT(from->getPath() == std::string(cli_name.data() + 1, cli_name.size() - 1));
    EXPECT(from->getAddrLen() == offsetof(sockaddr_un, sun_path) + cli_name.size());

    const std::string reply = "reply";
    EXPECT(srv->sendTo(reply.data(), reply.size(), from) == (int)reply.size());
    EXPECT(wait_fd(cli->getSocket(), POLLIN, 3000));

    char rbuf[64] = {0};
    iovec riov;
    riov.iov_base = rbuf;
    riov.iov_len = sizeof(rbuf) - 1;
    bronx::BxUnixAddress::ptr from2(new bronx::BxUnixAddress);
    int rn = cli->recvfrom(&riov, 1, from2);
    EXPECT(rn == (int)reply.size());
    EXPECT(std::string(rbuf, rn > 0 ? rn : 0) == reply);
    EXPECT(from2->getPath() == std::string(srv_name.data() + 1, srv_name.size() - 1));
    EXPECT(from2->getAddrLen() == offsetof(sockaddr_un, sun_path) + srv_name.size());

    cli->close();
    srv->close();
}
#endif


// ========== 五、getOption / setOption ==========
static void test_options(){
    BRONX_LOG_INFO(g_logger) << "--- test_options ---";

    auto s = bronx::BxSocket::MakeTcpSocket();
    // bind 一下触发 newSock，使 fd 真正存在
    auto a = bronx::BxIpAddress::Create("127.0.0.1", 0);
    EXPECT(s->bind(a));
    EXPECT(s->isValid());

    // SO_REUSEADDR（initSock 已设过，这里读回应为非 0）
    {
        int v = 0;
        EXPECT(s->getOption(SOL_SOCKET, SO_REUSEADDR, v));   // 模板版本
        EXPECT(v != 0);
    }
    // SO_RCVBUF / SO_SNDBUF：设置后读回（Linux 会翻倍，所以只验证 >= 期望）
    {
        int want = 64 * 1024;
        EXPECT(s->setOption(SOL_SOCKET, SO_RCVBUF, want));
        int got = 0;
        EXPECT(s->getOption(SOL_SOCKET, SO_RCVBUF, got));
        EXPECT(got >= want);
    }
    {
        int want = 64 * 1024;
        EXPECT(s->setOption(SOL_SOCKET, SO_SNDBUF, want));
        int got = 0;
        EXPECT(s->getOption(SOL_SOCKET, SO_SNDBUF, got));
        EXPECT(got >= want);
    }
    // TCP_NODELAY
    {
        int on = 1;
        EXPECT(s->setOption(IPPROTO_TCP, TCP_NODELAY, on));
        int got = 0;
        EXPECT(s->getOption(IPPROTO_TCP, TCP_NODELAY, got));
        EXPECT(got != 0);
    }
    // SO_ERROR：未发生错误应为 0
    {
        int err = -1;
        EXPECT(s->getOption(SOL_SOCKET, SO_ERROR, err));
        EXPECT(err == 0);
    }
    // 模板 setOption<T> + getOption<T> 的 timeval（间接验证 SO_RCVTIMEO）
    {
        struct timeval tv{ 1, 500000 };
        EXPECT(s->setOption(SOL_SOCKET, SO_RCVTIMEO, tv));
        struct timeval got{};
        EXPECT(s->getOption(SOL_SOCKET, SO_RCVTIMEO, got));
        EXPECT(got.tv_sec == 1);
    }
    // 非 BxIoManager 线程内，BxSocket 自身也应同步 BxFdCtx 里的超时记账。
    {
        s->setRecvTimeout(1234);
        EXPECT(s->getRecvTimeout() == 1234);
        s->setSendTimeout(2345);
        EXPECT(s->getSendTimeout() == 2345);
    }
    // ioctl(FIONBIO) 记录的是用户 nonblock 视图；底层 fd 仍保持系统非阻塞。
    {
        int on = 1;
        EXPECT(::ioctl(s->getSocket(), FIONBIO, &on) == 0);
        EXPECT((::fcntl(s->getSocket(), F_GETFL, 0) & O_NONBLOCK) != 0);
        EXPECT((fcntl_f(s->getSocket(), F_GETFL, 0) & O_NONBLOCK) != 0);

        int off = 0;
        EXPECT(::ioctl(s->getSocket(), FIONBIO, &off) == 0);
        EXPECT((::fcntl(s->getSocket(), F_GETFL, 0) & O_NONBLOCK) == 0);
        EXPECT((fcntl_f(s->getSocket(), F_GETFL, 0) & O_NONBLOCK) != 0);
    }

    // 无效 fd 上 getOption/setOption 应失败
    {
        auto bad = bronx::BxSocket::MakeTcpSocket();   // m_sock == -1
        int v = 0;
        EXPECT(!bad->getOption(SOL_SOCKET, SO_REUSEADDR, v));
        EXPECT(!bad->setOption(SOL_SOCKET, SO_REUSEADDR, v));
    }

    s->close();
}


// ========== 六、超时 ==========
// 注意：setRecvTimeout/getRecvTimeout 的"写内核 + 记账到 BxFdCtx"语义依赖 hook，
// 而 hook 只在 scheduler/BxIoManager 线程内启用。所以必须在协程里测。
static void test_timeout(){
    BRONX_LOG_INFO(g_logger) << "--- test_timeout ---";

    std::atomic<bool> done{false};
    {
        bronx::BxIoManager iom(1, "timeout-test");
        iom.post([&](){
            auto s = bronx::BxSocket::MakeTcpSocket();
            auto a = bronx::BxIpAddress::Create("127.0.0.1", 0);
            EXPECT(s->bind(a));

            // 设置后能从 BxFdCtx 读回（hook 已启用）
            s->setRecvTimeout(1500);
            EXPECT(s->getRecvTimeout() == 1500);
            s->setSendTimeout(2500);
            EXPECT(s->getSendTimeout() == 2500);
            // 旧拼写别名仍可用
            s->setSendTimeout(3000);
            EXPECT(s->getSendTimeout() == 3000);

            // recv 超时：连上 server 但对端不发数据，recv 应在 ~300ms 超时返回 -1
            auto server = bronx::BxSocket::MakeTcpSocket();
            EXPECT(server->bind(bronx::BxIpAddress::Create("127.0.0.1", 0)));
            EXPECT(server->listen());
            uint16_t port = std::dynamic_pointer_cast<bronx::BxIpAddress>(server->getLocalAddress())->getPort();

            auto client = bronx::BxSocket::MakeTcpSocket();
            EXPECT(client->connect(bronx::BxIpAddress::Create("127.0.0.1", port)));
            auto accepted = server->accept();
            EXPECT(accepted);

            client->setRecvTimeout(300);
            auto t0 = std::chrono::steady_clock::now();
            char buf[16];
            int n = client->recv(buf, sizeof(buf));   // 对端不发，触发超时
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            EXPECT(n < 0);                 // 超时返回失败
            EXPECT(ms >= 200 && ms < 2000); // 大致在设定超时附近
            // 超时后 socket 仍可控：能正常 close
            client->close();
            if(accepted) accepted->close();
            server->close();
            s->close();
            done = true;
        });
        iom.stop();
    }
    EXPECT(done.load());
}

// ========== 九、close 行为 ==========
static void test_close(){
    BRONX_LOG_INFO(g_logger) << "--- test_close ---";

    // 未创建 fd 的 TCP socket close：幂等成功
    {
        auto s = bronx::BxSocket::MakeTcpSocket();
        EXPECT(s->close());          // 修复后成功返回 true
        EXPECT(s->close());          // 再次 close 仍成功（幂等）
        EXPECT(!s->isValid());
    }
    // 已 bind/listen 未连接的监听 socket close
    {
        auto s = bronx::BxSocket::MakeTcpSocket();
        auto a = bronx::BxIpAddress::Create("127.0.0.1", 0);
        EXPECT(s->bind(a));
        EXPECT(s->listen());
        EXPECT(s->isValid());
        EXPECT(s->close());          // 成功
        EXPECT(s->getSocket() == -1);
        EXPECT(!s->isConnected());
        EXPECT(s->close());          // 幂等
    }
    // close 后本地地址缓存必须失效，避免后续 getLocalAddress 返回旧 fd 的地址
    {
        auto server = bronx::BxSocket::MakeTcpSocket();
        EXPECT(server->bind(bronx::BxIpAddress::Create("127.0.0.1", 0)));
        EXPECT(server->listen());
        uint16_t port = std::dynamic_pointer_cast<bronx::BxIpAddress>(server->getLocalAddress())->getPort();

        std::thread srv_th([&](){
            auto cli = accept_with_wait(server, 3000);
            if(cli) cli->close();
        });

        auto client = bronx::BxSocket::MakeTcpSocket();
        EXPECT(client->connect(bronx::BxIpAddress::Create("127.0.0.1", port)));
        EXPECT(std::dynamic_pointer_cast<bronx::BxIpAddress>(client->getLocalAddress()));
        EXPECT(client->close());
        auto after_close = client->getLocalAddress();
        EXPECT(after_close);
        EXPECT(!std::dynamic_pointer_cast<bronx::BxIpAddress>(after_close));

        srv_th.join();
        server->close();
    }
    // 同一个 BxSocket 从已连接客户端复用为监听 socket 时，bind 必须清掉旧 remote 缓存
    {
        auto server = bronx::BxSocket::MakeTcpSocket();
        EXPECT(server->bind(bronx::BxIpAddress::Create("127.0.0.1", 0)));
        EXPECT(server->listen());
        uint16_t port = std::dynamic_pointer_cast<bronx::BxIpAddress>(server->getLocalAddress())->getPort();

        std::thread srv_th([&](){
            auto cli = accept_with_wait(server, 3000);
            if(cli) cli->close();
        });

        auto reused = bronx::BxSocket::MakeTcpSocket();
        EXPECT(reused->connect(bronx::BxIpAddress::Create("127.0.0.1", port)));
        EXPECT(reused->toString().find("remote_address=") != std::string::npos);
        EXPECT(reused->close());
        srv_th.join();
        server->close();

        EXPECT(reused->bind(bronx::BxIpAddress::Create("127.0.0.1", 0)));
        EXPECT(reused->listen());
        EXPECT(reused->toString().find("remote_address=") == std::string::npos);
        reused->close();
    }
}


// ========== 十、reconnect ==========
static void test_reconnect(){
    BRONX_LOG_INFO(g_logger) << "--- test_reconnect ---";

    // 没有 remote 地址时 reconnect 必失败
    {
        auto s = bronx::BxSocket::MakeTcpSocket();
        EXPECT(!s->reconnect());
    }

    // connect 成功 → close → reconnect 应能重新连上（依赖 m_remoteAddress 缓存正确）
    auto server = bronx::BxSocket::MakeTcpSocket();
    auto a = bronx::BxIpAddress::Create("127.0.0.1", 0);
    EXPECT(server->bind(a));
    EXPECT(server->listen());
    uint16_t port = std::dynamic_pointer_cast<bronx::BxIpAddress>(server->getLocalAddress())->getPort();

    std::atomic<int> accepted{0};
    std::thread srv_th([&](){
        for(int i = 0; i < 2; ++i){
            auto cli = accept_with_wait(server, 3000);
            if(cli){ ++accepted; cli->close(); }
        }
    });

    auto client = bronx::BxSocket::MakeTcpSocket();
    auto target = bronx::BxIpAddress::Create("127.0.0.1", port);
    EXPECT(client->connect(target));
    EXPECT(client->isConnected());
    // reconnect 之前 remote 必须已经缓存为 server 地址（回归 connect 的 m_remoteAddress）
    auto rem = std::dynamic_pointer_cast<bronx::BxIpAddress>(client->getRemoteAddress());
    EXPECT(rem && rem->getPort() == port);

    client->close();
    EXPECT(client->reconnect());     // 用缓存的 remote 地址重连
    EXPECT(client->isConnected());

    client->close();
    srv_th.join();
    EXPECT(accepted.load() == 2);
    server->close();
}


// ========== 十二、错误路径 / 边界 ==========
static void test_error_paths(){
    BRONX_LOG_INFO(g_logger) << "--- test_error_paths ---";

    // family 不匹配：IPv4 socket bind 到 IPv6 地址应失败
    {
        auto s = bronx::BxSocket::MakeTcpSocket();    // AF_INET
        auto v6 = bronx::BxIpAddress::Create("::1", 0); // AF_INET6
        if(v6){
            EXPECT(!s->bind(v6));
        }
        s->close();
    }
    // connect 到一个没人监听的端口应失败
    {
        // 先占一个端口拿到号，然后关闭，去 connect 这个大概率没人听的端口
        auto probe = bronx::BxSocket::MakeTcpSocket();
        auto a = bronx::BxIpAddress::Create("127.0.0.1", 0);
        probe->bind(a);
        uint16_t freed = std::dynamic_pointer_cast<bronx::BxIpAddress>(probe->getLocalAddress())->getPort();
        probe->close();

        auto c = bronx::BxSocket::MakeTcpSocket();
        auto target = bronx::BxIpAddress::Create("127.0.0.1", freed);
        EXPECT(!c->connect(target, 500));   // 带超时，避免长时间阻塞
        c->close();
    }
    // bind 已占用端口应失败
    {
        auto s1 = bronx::BxSocket::MakeTcpSocket();
        auto a = bronx::BxIpAddress::Create("127.0.0.1", 0);
        EXPECT(s1->bind(a));
        EXPECT(s1->listen());
        uint16_t port = std::dynamic_pointer_cast<bronx::BxIpAddress>(s1->getLocalAddress())->getPort();

        // s2 关掉 REUSEADDR 后 bind 同端口应失败
        auto s2 = bronx::BxSocket::MakeTcpSocket();
        s2->bind(bronx::BxIpAddress::Create("127.0.0.1", 0)); // 先 newSock
        int off = 0;
        s2->setOption(SOL_SOCKET, SO_REUSEADDR, off);
        auto same = bronx::BxIpAddress::Create("127.0.0.1", port);
        // 注意 s2 已 bind 过一次，再 bind 行为依赖实现；这里用全新 socket 更干净
        auto s3 = bronx::BxSocket::MakeTcpSocket();
        // s3 未设置 REUSEADDR 之前 initSock 默认开了 REUSEADDR；
        // 但已被 LISTEN 占用的端口，二次 bind 仍会 EADDRINUSE
        EXPECT(!s3->bind(same));
        s1->close(); s2->close(); s3->close();
    }
    // 未连接 TCP socket 上 send/recv 失败
    {
        auto s = bronx::BxSocket::MakeTcpSocket();
        EXPECT(s->send("x", 1) < 0);
        char b[4];
        EXPECT(s->recv(b, sizeof(b)) < 0);
        s->close();
    }
    // 空地址参数：返回失败，不崩溃
    {
        auto tcp = bronx::BxSocket::MakeTcpSocket();
        EXPECT(!tcp->bind(nullptr));
        EXPECT(!tcp->connect(nullptr));
        tcp->close();

        auto udp = bronx::BxSocket::MakeUdpSocket();
        EXPECT(udp->sendTo("x", 1, nullptr) < 0);
        char b[8];
        EXPECT(udp->recvfrom(b, sizeof(b), nullptr) < 0);
        iovec iov;
        iov.iov_base = b;
        iov.iov_len = sizeof(b);
        EXPECT(udp->recvfrom(&iov, 1, nullptr) < 0);
        udp->close();
    }
    // 非 BxIoManager 线程调用 cancel API：安全返回 false，不应解引用空调度器
    {
        auto s = bronx::BxSocket::MakeTcpSocket();
        EXPECT(!s->abortRead());
        EXPECT(!s->abortWrite());
        EXPECT(!s->abortAccept());
        EXPECT(!s->abortAll());
        s->close();
    }
}


// ========== 对端关闭后的收发行为 ==========
static void test_peer_close(){
    BRONX_LOG_INFO(g_logger) << "--- test_peer_close ---";

    auto server = bronx::BxSocket::MakeTcpSocket();
    auto a = bronx::BxIpAddress::Create("127.0.0.1", 0);
    EXPECT(server->bind(a));
    EXPECT(server->listen());
    uint16_t port = std::dynamic_pointer_cast<bronx::BxIpAddress>(server->getLocalAddress())->getPort();

    std::thread srv_th([&](){
        auto cli = accept_with_wait(server, 3000);
        EXPECT(cli);
        if(cli){
            // 立刻关闭，制造对端 EOF
            cli->close();
        }
    });

    auto client = bronx::BxSocket::MakeTcpSocket();
    EXPECT(client->connect(bronx::BxIpAddress::Create("127.0.0.1", port)));
    // 对端 close 后，recv 应读到 0（EOF）
    char buf[16];
    int n = recv_with_wait(client, buf, sizeof(buf), 3000);
    EXPECT(n == 0);

    srv_th.join();
    client->close();
    server->close();
}

// ========== BxSocketStream + BxByteArray ==========
static void test_socket_stream_bytearray(){
    BRONX_LOG_INFO(g_logger) << "--- test_socket_stream_bytearray ---";

    auto server = bronx::BxSocket::MakeTcpSocket();
    EXPECT(server->bind(bronx::BxIpAddress::Create("127.0.0.1", 0)));
    EXPECT(server->listen());
    uint16_t port = std::dynamic_pointer_cast<bronx::BxIpAddress>(server->getLocalAddress())->getPort();

    std::string payload;
    payload.resize(4096);
    for(size_t i = 0; i < payload.size(); ++i){
        payload[i] = 'a' + (i % 26);
    }
    const std::string chunk_payload = "ABCDEFGHIJKLMNOP";

    std::atomic<bool> srv_ok{false};
    std::thread srv_th([&](){
        auto accepted = accept_with_wait(server, 3000);
        EXPECT(accepted);
        if(!accepted){
            return;
        }

        bronx::BxSocketStream stream(accepted, false);
        bronx::BxByteArray::ptr null_ba;
        EXPECT(stream.read(null_ba, 1) < 0);
        EXPECT(wait_fd(accepted->getSocket(), POLLIN, 3000));

        bronx::BxByteArray::ptr rx(new bronx::BxByteArray(16));
        int n = stream.read(rx, payload.size());
        EXPECT(n == (int)payload.size());
        if(n > 0){
            rx->setPosition(0);
            EXPECT(rx->toString() == payload);
        }
        EXPECT(stream.read(rx, 0) == 0);

        bronx::BxByteArray::ptr small(new bronx::BxByteArray(8));
        EXPECT(wait_fd(accepted->getSocket(), POLLIN, 3000));
        EXPECT(stream.read(small, 8) == 8);
        EXPECT(wait_fd(accepted->getSocket(), POLLIN, 3000));
        EXPECT(stream.read(small, 8) == 8);
        small->setPosition(0);
        EXPECT(small->toString() == chunk_payload);

        srv_ok = true;
        accepted->close();
    });

    auto client = bronx::BxSocket::MakeTcpSocket();
    EXPECT(client->connect(bronx::BxIpAddress::Create("127.0.0.1", port)));
    bronx::BxSocketStream stream(client, false);
    bronx::BxByteArray::ptr null_ba;
    EXPECT(stream.write(null_ba, 1) < 0);

    bronx::BxByteArray::ptr tx(new bronx::BxByteArray(16));
    tx->write(payload.data(), payload.size());
    tx->setPosition(0);
    EXPECT(stream.write(tx, 0) == 0);
    EXPECT(stream.writeExact(tx, payload.size()) == (int)payload.size());
    EXPECT(stream.writeExact(chunk_payload.data(), chunk_payload.size()) == (int)chunk_payload.size());

    srv_th.join();
    EXPECT(srv_ok.load());

    client->close();
    server->close();
}


// ========== 十三、稳定性：多连接 ==========
static void test_stress(){
    BRONX_LOG_INFO(g_logger) << "--- test_stress ---";

    auto server = bronx::BxSocket::MakeTcpSocket();
    auto a = bronx::BxIpAddress::Create("127.0.0.1", 0);
    EXPECT(server->bind(a));
    EXPECT(server->listen());
    uint16_t port = std::dynamic_pointer_cast<bronx::BxIpAddress>(server->getLocalAddress())->getPort();

    const int N = 200;
    std::atomic<int> served{0};
    std::thread srv_th([&](){
        for(int i = 0; i < N; ++i){
            auto cli = accept_with_wait(server, 3000);
            if(!cli){ break; }
            char buf[32] = {0};
            int n = recv_with_wait(cli, buf, sizeof(buf)-1, 2000);
            if(n > 0){
                cli->send(buf, n);   // echo
                ++served;
            }
            cli->close();
        }
    });

    int ok = 0;
    for(int i = 0; i < N; ++i){
        auto c = bronx::BxSocket::MakeTcpSocket();
        if(!c->connect(bronx::BxIpAddress::Create("127.0.0.1", port), 1000)){
            continue;
        }
        std::string msg = "n" + std::to_string(i);
        if(c->send(msg.data(), msg.size()) != (int)msg.size()){ c->close(); continue; }
        char buf[32] = {0};
        int n = recv_with_wait(c, buf, sizeof(buf)-1, 2000);
        if(n == (int)msg.size() && std::string(buf, n) == msg){
            ++ok;
        }
        c->close();
    }

    srv_th.join();
    BRONX_LOG_INFO(g_logger) << "stress: ok=" << ok << "/" << N << " served=" << served.load();
    EXPECT(ok == N);
    EXPECT(served.load() == N);
    server->close();
}

// ========== 十一、cancel API（BxIoManager 协程内）==========
// 验证 abortAccept / abortRead 能真正【唤醒并退出】一个无数据而挂起的协程，
// 不依赖 close 或超时兜底（这是修复 do_io/cancel 协作缺陷后应有的行为）。
//   场景 A：协程在监听 socket 上 accept 挂起 → abortAccept → accept 返回 nullptr 并退出
//   场景 B：协程在 connected socket 上 recv 挂起 → abortRead → recv 返回 -1 并退出
//   附加：cancel 不等于 close —— cancel 后 socket 仍能正常 close
static void test_cancel(){
    BRONX_LOG_INFO(g_logger) << "--- test_cancel ---";

    std::atomic<bool> accept_woken{false};
    std::atomic<bool> recv_woken{false};
    std::atomic<bool> invalid_cancel_done{false};
    std::atomic<int>  accept_cancel_ret{-100};
    std::atomic<int>  read_cancel_ret{-100};

    {
        // 多 worker：验证 cancel 在多线程 BxIoManager 下也能正确唤醒并干净退出
        bronx::BxIoManager iom(2, "cancel-test");

        iom.post([&](){
            auto s = bronx::BxSocket::MakeTcpSocket(); // m_sock == -1
            EXPECT(!s->abortRead());
            EXPECT(!s->abortWrite());
            EXPECT(!s->abortAccept());
            EXPECT(!s->abortAll());
            invalid_cancel_done = true;
        });

        // 场景 A：abortAccept 唤醒 accept
        iom.post([&](){
            auto srv = bronx::BxSocket::MakeTcpSocket();
            EXPECT(srv->bind(bronx::BxIpAddress::Create("127.0.0.1", 0)));
            EXPECT(srv->listen());

            bronx::BxIoManager::Current()->addTimer(200, [&, srv](){
                accept_cancel_ret = srv->abortAccept() ? 1 : 0;
            });

            auto cli = srv->accept();   // 无连接 → 挂起 → 被 abortAccept 唤醒
            EXPECT(!cli);               // 取消后 accept 返回空
            accept_woken = true;
            // cancel 之后仍可正常 close
            EXPECT(srv->close());
        });

        // 场景 B：abortRead 唤醒 recv
        iom.post([&](){
            auto server = bronx::BxSocket::MakeTcpSocket();
            EXPECT(server->bind(bronx::BxIpAddress::Create("127.0.0.1", 0)));
            EXPECT(server->listen());
            uint16_t port = std::dynamic_pointer_cast<bronx::BxIpAddress>(server->getLocalAddress())->getPort();

            auto client = bronx::BxSocket::MakeTcpSocket();
            EXPECT(client->connect(bronx::BxIpAddress::Create("127.0.0.1", port)));
            auto accepted = server->accept();
            EXPECT(accepted);

            bronx::BxIoManager::Current()->addTimer(200, [&, client](){
                read_cancel_ret = client->abortRead() ? 1 : 0;
            });

            char buf[16];
            int n = client->recv(buf, sizeof(buf));   // 无数据 → 挂起 → 被 abortRead 唤醒
            EXPECT(n < 0);                             // 取消后返回失败
            recv_woken = true;

            EXPECT(client->close());                  // cancel 后仍可 close
            if(accepted) accepted->close();
            server->close();
        });

        iom.stop();
    }

    EXPECT(accept_woken.load());            // accept 协程被唤醒并退出
    EXPECT(recv_woken.load());              // recv 协程被唤醒并退出
    EXPECT(invalid_cancel_done.load());     // BxIoManager 内无 fd cancel 安全失败
    EXPECT(accept_cancel_ret.load() == 1);  // abortAccept 返回 true
    EXPECT(read_cancel_ret.load() == 1);    // abortRead 返回 true
}

// 跑一个用例并在其后强制复位主线程 hook 状态。
// BxScheduler::run 已经用 RAII 恢复 hook；这里保留复位作为测试隔离，避免单个用例
// 手动切换 hook 状态后污染后续用例。
static void run_case(void(*fn)()){
    bronx::set_hook_enable(false);
    fn();
    bronx::set_hook_enable(false);
}

int main(){
    BRONX_LOG_INFO(g_logger) << "==== BxSocket module test start ====";

    run_case(test_factory);      // 一、工厂
    run_case(test_tcp_v4);       // 二、TCP IPv4
    run_case(test_tcp_v6);       // 三、TCP IPv6（可跳过）
    run_case(test_udp_v4);       // 四、UDP IPv4 + recvfrom 长度回归
#ifdef __linux__
    run_case(test_unix_udp);     // 四 b、Unix UDP + abstract 来源地址长度
#endif
    run_case(test_options);      // 五、get/setOption
    run_case(test_timeout);      // 六、超时（BxIoManager 内）
    // 七、local/remote 地址：已并入 tcp_roundtrip
    // 八、accept/init：已并入 tcp_roundtrip（accepted socket 校验）
    run_case(test_close);        // 九、close 行为
    run_case(test_reconnect);    // 十、reconnect
    run_case(test_cancel);       // 十一、cancel（BxIoManager 内）
    run_case(test_error_paths);  // 十二、错误路径
    run_case(test_peer_close);   // 对端关闭
    run_case(test_socket_stream_bytearray); // BxSocketStream + BxByteArray iovec
    run_case(test_stress);       // 十三、稳定性

    BRONX_LOG_INFO(g_logger) << "==== checks=" << g_checks
                             << " failed=" << g_failed << " ====";
    if(g_failed){
        BRONX_LOG_ERROR(g_logger) << "SOCKET TESTS FAILED: " << g_failed;
        return 1;
    }
    BRONX_LOG_INFO(g_logger) << "ALL SOCKET TESTS PASS";
    return 0;
}
