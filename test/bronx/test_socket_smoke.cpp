// BxSocket 模块「日常使用」冒烟测试
// ================================
// 目标：用框架的主消费者 BxTcpServer 跑一个真实 echo 服务，串起
//   BxSocket + BxIoManager + hook + fiber 完整链路，验证常规路径下稳定可用。
//
// 结构：
//   - server：EchoServer(BxTcpServer 子类) 跑在后台 BxIoManager 线程池里
//   - client：主线程用裸 POSIX 阻塞 socket 连接 loopback，多轮/多并发收发
//   - 不依赖公网、不用固定端口（:0 自动分配，getLocalAddress 取真实端口）
//
// 客户端故意用裸 socket（而非 bronx::BxSocket）：bronx 客户端 fd 默认非阻塞且
// hook 仅在协程线程生效，裸 socket 在主线程做阻塞收发最贴近“外部用户”视角，
// 也能独立校验 server 端的行为。

#include "tcp_listener.h"
#include "reactor.h"
#include "net_socket.h"
#include "endpoint.h"
#include "log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();
static std::atomic<int> g_failed{0};
static std::atomic<int> g_checks{0};

#define EXPECT(expr)                                                          \
    do {                                                                      \
        ++g_checks;                                                           \
        if(!(expr)) {                                                         \
            BRONX_LOG_ERROR(g_logger) << "FAIL: " #expr                       \
                                      << " @ " << __FILE__ << ":" << __LINE__;\
            ++g_failed;                                                       \
        }                                                                     \
    } while(0)


// echo 服务：收到啥回啥，直到对端关闭
class EchoServer : public bronx::BxTcpServer {
public:
    EchoServer(bronx::BxIoManager* io, bronx::BxIoManager* accept)
        : bronx::BxTcpServer(io, accept) {}

    std::atomic<int> conn_count{0};

protected:
    void onConnection(bronx::BxSocket::ptr client) override {
        ++conn_count;
        std::vector<char> buf(4096);
        while(true){
            int n = client->recv(buf.data(), buf.size());
            if(n <= 0){
                break;                 // 0=对端关闭, <0=错误/超时
            }
            // 原样回发（处理短写）
            int sent = 0;
            while(sent < n){
                int w = client->send(buf.data() + sent, n - sent);
                if(w <= 0){ break; }
                sent += w;
            }
        }
        client->close();
    }
};


// 一个裸 socket 阻塞客户端：connect→发若干轮→校验回显→关闭
// 返回成功完成的轮数
static int run_blocking_client(uint16_t port, int rounds){
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if(::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0){
        ::close(fd);
        return -1;
    }

    int ok = 0;
    for(int i = 0; i < rounds; ++i){
        std::string msg = "round-" + std::to_string(i) + "-payload";
        if(::send(fd, msg.data(), msg.size(), 0) != (ssize_t)msg.size()){
            break;
        }
        // 读回显（echo 长度等于发送长度，可能分片，循环读满）
        std::string got;
        got.reserve(msg.size());
        char rbuf[256];
        while(got.size() < msg.size()){
            ssize_t n = ::recv(fd, rbuf, sizeof(rbuf), 0);
            if(n <= 0){ break; }
            got.append(rbuf, n);
        }
        if(got == msg){
            ++ok;
        } else {
            break;
        }
    }
    ::close(fd);
    return ok;
}


// 启动 echo server，返回监听端口；server 在 io_mgr 后台线程池里运行
static uint16_t start_echo_server(std::shared_ptr<EchoServer>& srv,
                                  bronx::BxIoManager* io_mgr){
    auto addr = bronx::BxIpAddress::Create("127.0.0.1", 0);
    srv = std::make_shared<EchoServer>(io_mgr, io_mgr);
    bool bound = false;
    // bind/start 必须在 BxIoManager 线程里发起（acceptLoop 会被 post）
    io_mgr->post([&, addr](){
        bound = srv->bind(addr);
        if(bound){
            srv->start();
        }
    });
    // 等 bind 完成
    for(int i = 0; i < 200 && !bound; ++i){
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if(!bound) return 0;
    auto la = std::dynamic_pointer_cast<bronx::BxIpAddress>(srv->getSocks()[0]->getLocalAddress());
    return la ? la->getPort() : 0;
}


// ===== 场景一：单客户端、多轮顺序收发 =====
static void test_sequential(bronx::BxIoManager* io){
    BRONX_LOG_INFO(g_logger) << "--- test_sequential ---";
    std::shared_ptr<EchoServer> srv;
    uint16_t port = start_echo_server(srv, io);
    EXPECT(port != 0);
    if(port){
        int ok = run_blocking_client(port, 20);
        EXPECT(ok == 20);     // 20 轮全部回显正确
    }
    srv->stop();
}


// ===== 场景二：多客户端并发 =====
static void test_concurrent(bronx::BxIoManager* io){
    BRONX_LOG_INFO(g_logger) << "--- test_concurrent ---";
    std::shared_ptr<EchoServer> srv;
    uint16_t port = start_echo_server(srv, io);
    EXPECT(port != 0);
    if(port){
        const int N = 50;
        std::vector<std::thread> clients;
        std::atomic<int> success{0};
        for(int i = 0; i < N; ++i){
            clients.emplace_back([&](){
                if(run_blocking_client(port, 5) == 5){
                    ++success;
                }
            });
        }
        for(auto& t : clients){ t.join(); }
        BRONX_LOG_INFO(g_logger) << "concurrent: success=" << success.load() << "/" << N;
        EXPECT(success.load() == N);
        EXPECT(srv->conn_count.load() == N);
    }
    srv->stop();
}


// ===== 场景三：短连接快速建立/关闭（连接搅动）=====
static void test_churn(bronx::BxIoManager* io){
    BRONX_LOG_INFO(g_logger) << "--- test_churn ---";
    std::shared_ptr<EchoServer> srv;
    uint16_t port = start_echo_server(srv, io);
    EXPECT(port != 0);
    if(port){
        int ok = 0;
        const int N = 100;
        for(int i = 0; i < N; ++i){
            if(run_blocking_client(port, 1) == 1){ ++ok; }
        }
        BRONX_LOG_INFO(g_logger) << "churn: ok=" << ok << "/" << N;
        EXPECT(ok == N);
    }
    srv->stop();
}


int main(){
    BRONX_LOG_INFO(g_logger) << "==== BxSocket daily-use smoke test start ====";

    {
        // 后台 BxIoManager:2 个工作线程,主线程作为客户端(去 use_caller 后主线程本就不参与调度)
        bronx::BxIoManager io(2, "echo-io");

        test_sequential(&io);
        test_concurrent(&io);
        test_churn(&io);

        // 给后台 stop 调度一点时间，然后 BxIoManager 析构会 stop 并 join
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    BRONX_LOG_INFO(g_logger) << "==== checks=" << g_checks.load()
                             << " failed=" << g_failed.load() << " ====";
    if(g_failed.load()){
        BRONX_LOG_ERROR(g_logger) << "SMOKE TEST FAILED: " << g_failed.load();
        return 1;
    }
    BRONX_LOG_INFO(g_logger) << "ALL SMOKE TESTS PASS";
    return 0;
}
