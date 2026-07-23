// BxTcpServer 连接生命周期回归测试(阶段3)
// =======================================
// 守护连接表 / stop / drain / maxConnections:
//   1. 多客户端连接 → activeConnectionCount 增;客户端关闭 → 减
//   2. server.stop() → 活跃连接被关闭、计数归零
//   3. drain() → isDraining 置位、超时强关
//   4. maxConnections=1 → 第二个连接被拒,active 不超 1
// 客户端用裸阻塞 socket(主线程不在 BxIoManager,真阻塞 connect/read 没问题)。

#include "test_util.h"
#include "tcp_listener.h"
#include "reactor.h"
#include "endpoint.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

// onConnection 阻塞读到对端关闭/出错才返回 —— 让连接在客户端保持期间一直处于活跃表
class HoldServer : public bronx::BxTcpServer {
public:
    using bronx::BxTcpServer::BxTcpServer;
protected:
    void onConnection(bronx::BxSocket::ptr client) override {
        char buf[64];
        while(true){
            int rt = client->recv(buf, sizeof(buf));
            if(rt <= 0) break;   // 对端关闭 / 错误 / 被 abortAll 唤醒
        }
        client->close();
    }
};

// 裸阻塞客户端:连到 127.0.0.1:port
static int connect_client(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if(connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0){
        close(fd);
        return -1;
    }
    return fd;
}

static const uint16_t PORT = 18421;

static uint16_t reserve_port(std::unique_ptr<int, void(*)(int*)>& holder) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_CHECK_MSG(fd >= 0, "reserve socket failed");
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    TEST_CHECK_MSG(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0, "reserve bind failed");
    TEST_CHECK_MSG(listen(fd, 16) == 0, "reserve listen failed");
    socklen_t len = sizeof(addr);
    TEST_CHECK_MSG(getsockname(fd, (sockaddr*)&addr, &len) == 0, "getsockname failed");
    holder.reset(new int(fd));
    return ntohs(addr.sin_port);
}

// 1. 连接计数增减 + stop 归零
static void test_count_and_stop() {
    bronx::BxIoManager iom(2);
    HoldServer::ptr server(new HoldServer(&iom, &iom));
    auto addr = bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(PORT));
    TEST_CHECK_MSG(server->bind(addr), "bind failed");
    server->start();
    usleep(100 * 1000);

    // 建立 3 个客户端
    std::vector<int> clients;
    for(int i = 0; i < 3; ++i){
        int fd = connect_client(PORT);
        TEST_CHECK_MSG(fd >= 0, "client connect failed");
        if(fd >= 0) clients.push_back(fd);
    }
    usleep(200 * 1000);  // 等 accept + onConnection 登记
    TEST_CHECK_EQ((int)server->getActiveConnectionCount(), 3);

    // 关掉 1 个客户端 → 活跃数降到 2
    close(clients[0]);
    usleep(200 * 1000);
    TEST_CHECK_EQ((int)server->getActiveConnectionCount(), 2);

    // server.stop() → 剩余连接被关闭,计数归零
    server->stop();
    usleep(300 * 1000);
    TEST_CHECK_EQ((int)server->getActiveConnectionCount(), 0);

    for(size_t i = 1; i < clients.size(); ++i) close(clients[i]);
}

// 2. maxConnections=1 拒绝超额连接
static void test_max_connections() {
    bronx::BxIoManager iom(2);
    HoldServer::ptr server(new HoldServer(&iom, &iom));
    bronx::TcpServerOptions opt;
    opt.maxConnections = 1;
    server->setOptions(opt);
    auto addr = bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(PORT + 1));
    TEST_CHECK_MSG(server->bind(addr), "bind failed");
    server->start();
    usleep(100 * 1000);

    int c1 = connect_client(PORT + 1);
    int c2 = connect_client(PORT + 1);   // 第二个应被服务端 accept 后立即 close
    usleep(300 * 1000);

    // 活跃连接不超过 1
    TEST_CHECK_MSG(server->getActiveConnectionCount() <= 1,
        "active=" << server->getActiveConnectionCount() << " (max should be 1)");
    auto st = server->getStats();
    TEST_CHECK_MSG(st.rejectedTotal >= 1, "expected >=1 rejected, got " << st.rejectedTotal);

    server->stop();
    usleep(200 * 1000);
    if(c1 >= 0) close(c1);
    if(c2 >= 0) close(c2);
}

// 3. drain 置位 + 超时强关
static void test_drain() {
    bronx::BxIoManager iom(2);
    HoldServer::ptr server(new HoldServer(&iom, &iom));
    auto addr = bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(PORT + 2));
    TEST_CHECK_MSG(server->bind(addr), "bind failed");
    server->start();
    usleep(100 * 1000);

    int c = connect_client(PORT + 2);
    usleep(200 * 1000);
    TEST_CHECK_EQ((int)server->getActiveConnectionCount(), 1);

    // drain(200ms):应立即置 draining,200ms 后强关连接
    server->drain(200);
    TEST_CHECK_MSG(server->isDraining(), "isDraining should be true after drain()");
    TEST_CHECK_MSG(server->waitDrain(1000), "drain should finish");
    TEST_CHECK_EQ((int)server->getActiveConnectionCount(), 0);

    server->stop();
    usleep(100 * 1000);
    if(c >= 0) close(c);
}

static void test_idle_drain() {
    uint64_t cost = 0;
    {
        bronx::BxIoManager iom(1);
        HoldServer::ptr server(new HoldServer(&iom, &iom));
        auto addr = bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(PORT + 3));
        TEST_CHECK_MSG(server->bind(addr), "bind failed");
        server->start();
        usleep(100 * 1000);

        auto begin = std::chrono::steady_clock::now();
        server->drain(2000);
        TEST_CHECK_MSG(server->waitDrain(100), "idle drain should finish");
        server->stop();
        server.reset();
        iom.stop();
        cost = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count();
    }
    TEST_CHECK_MSG(cost < 500, "idle drain waited " << cost << "ms");
}

// drain 和 stop 会分别投到 acceptWorker,多 worker 时也只能有一个任务取走监听 socket。
static void test_drain_then_stop_concurrently() {
    bronx::BxIoManager iom(4);
    for(int i = 0; i < 20; ++i) {
        HoldServer::ptr server(new HoldServer(&iom, &iom));
        auto addr = bronx::BxAddress::LookupAny("127.0.0.1:0");
        TEST_CHECK_MSG(server->bind(addr), "bind failed");
        server->start();
        server->drain();
        TEST_CHECK_MSG(server->waitDrain(100), "idle drain should finish");
        server->stop();

        for(int retry = 0; retry < 100 && !server->getSocks().empty(); ++retry) {
            usleep(1000);
        }
        TEST_CHECK_MSG(server->getSocks().empty(), "listen sockets should close once");
    }
}

// 4. 多地址 bind 失败时必须回填 fails,并清理已经成功绑定的监听 socket
static void test_bind_fail_cleanup() {
    bronx::BxIoManager iom(1);
    std::unique_ptr<int, void(*)(int*)> holder(nullptr, [](int* p){
        if(p){
            close(*p);
            delete p;
        }
    });
    uint16_t busy_port = reserve_port(holder);

    HoldServer::ptr server(new HoldServer(&iom, &iom));
    std::vector<bronx::BxAddress::ptr> addrs;
    addrs.push_back(bronx::BxAddress::LookupAny("127.0.0.1:0"));
    addrs.push_back(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(busy_port)));
    std::vector<bronx::BxAddress::ptr> fails;

    TEST_CHECK_MSG(!server->bind(addrs, fails), "multi bind should fail on busy port");
    TEST_CHECK_EQ((int)fails.size(), 1);
    TEST_CHECK_MSG(server->getSocks().empty(), "partially bound sockets were not cleared");
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_tcp_server_lifecycle start ===";
    test_count_and_stop();
    test_max_connections();
    test_drain();
    test_idle_drain();
    test_drain_then_stop_concurrently();
    test_bind_fail_cleanup();
    return TEST_SUMMARY();
}
