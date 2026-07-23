// G2 端到端:GatewayServer + GatewayConnection 连接 runtime
// =========================================================
// 真实 client→网关 往返,验证:请求解析、keep-alive 复用、body 读取(流式)、
// 准入限制(413/431)、drain 优雅退出、连接计数(白拿 BxTcpServer)。
// handler 用占位:回显请求方法+路径+body。

#include "test_util.h"
#include "gateway.h"
#include "conn.h"
#include "mempool.h"
#include "reactor.h"
#include "endpoint.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <atomic>
#include <cstdlib>
#include <cerrno>
#include <functional>

using namespace bronx::gateway;
static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

static const uint16_t PORT = 18600;
static constexpr size_t kObservedLargeAlloc = 64 * 1024;

// 占位 handler:读完 body,回 200,body = "method path|<reqbody>"
static void echo_handler(GatewayConnection& c) {
    auto req = c.request();
    std::string reqBody;
    auto reader = c.requestBody();
    if(reader) {
        int n;
        while((n = reader->readChunk(reqBody)) > 0) {}
    }
    auto rsp = std::make_shared<GwResponse>();
    rsp->setStatus(200);
    rsp->setHeader("Content-Type", "text/plain");
    std::string body = std::string(HttpMethodToString(req->getMethod()))
                     + " " + req->getPath() + "|" + reqBody;
    c.sendResponse(rsp, body);
}

// 裸阻塞客户端:连接、发原始报文、收响应(读到一定字节或对端关闭)
static std::string raw_exchange(int fd, const std::string& wire) {
    send(fd, wire.data(), wire.size(), 0);
    std::string resp;
    char buf[1024];
    // 读一次(响应小,通常一次到达)
    int n = recv(fd, buf, sizeof(buf), 0);
    if(n > 0) resp.assign(buf, n);
    return resp;
}

static bool send_all_fd(int fd, const std::string& wire) {
    size_t off = 0;
    while(off < wire.size()) {
        ssize_t n = send(fd, wire.data() + off, wire.size() - off, 0);
        if(n > 0) {
            off += (size_t)n;
            continue;
        }
        if(n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static bool recv_until_contains(int fd, const std::string& needle, std::string& resp) {
    if(resp.find(needle) != std::string::npos) return true;
    char buf[2048];
    for(int i = 0; i < 200; ++i) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if(n > 0) {
            resp.append(buf, n);
            if(resp.find(needle) != std::string::npos) return true;
            continue;
        }
        if(n == 0) return false;
        if(errno == EINTR) continue;
        return false;
    }
    return false;
}

struct AllocStats {
    std::atomic<int> largeAllocs{0};
    std::atomic<int> largeFrees{0};
};

static void reset_allocator() {
    MemAlloc::setAllocator(nullptr, nullptr);
}

static void install_observed_allocator(AllocStats* stats) {
    MemAlloc::setAllocator(
        [stats](size_t n) -> void* {
            if(n >= kObservedLargeAlloc) {
                stats->largeAllocs.fetch_add(1, std::memory_order_relaxed);
            }
            return ::operator new(n);
        },
        [stats](void* p, size_t n) {
            if(n >= kObservedLargeAlloc) {
                stats->largeFrees.fetch_add(1, std::memory_order_relaxed);
            }
            ::operator delete(p);
        });
}

static int connect_fd() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if(connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}

// 启动一个网关 server(在给定 iom 上),返回 server
static GatewayServer::ptr start_server(bronx::BxIoManager& iom, const GatewayOptions& opts) {
    auto server = std::make_shared<GatewayServer>(opts, &iom, &iom);
    server->setRequestHandler(echo_handler);
    auto addr = bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(PORT));
    server->bind(addr);
    server->start();
    return server;
}

// 1. 基本 GET 往返
static void test_basic_roundtrip() {
    bronx::BxIoManager iom(2);
    auto server = start_server(iom, GatewayOptions());
    usleep(150 * 1000);

    int fd = connect_fd();
    TEST_CHECK_MSG(fd >= 0, "connect");
    std::string resp = raw_exchange(fd,
        "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    TEST_CHECK_MSG(resp.find("200") != std::string::npos, "status 200: " << resp.substr(0, 20));
    TEST_CHECK_MSG(resp.find("GET /hello|") != std::string::npos, "echo body present");
    close(fd);
    server->stop();
    usleep(150 * 1000);
}

// 2. POST + Content-Length body 回显
static void test_post_body() {
    bronx::BxIoManager iom(2);
    auto server = start_server(iom, GatewayOptions());
    usleep(150 * 1000);

    int fd = connect_fd();
    std::string resp = raw_exchange(fd,
        "POST /submit HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
    TEST_CHECK_MSG(resp.find("|hello") != std::string::npos, "body echo: " << resp);
    close(fd);
    server->stop();
    usleep(150 * 1000);
}

// 2b. HEAD 应只返回响应头,Content-Length 表示对应 GET body 长度,不能发送 body。
static void test_head_no_body() {
    bronx::BxIoManager iom(2);
    auto server = start_server(iom, GatewayOptions());
    usleep(150 * 1000);

    int fd = connect_fd();
    std::string resp = raw_exchange(fd,
        "HEAD /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    TEST_CHECK_MSG(resp.find("200") != std::string::npos, "HEAD status 200: " << resp.substr(0, 40));
    TEST_CHECK_MSG(resp.find("Content-Length: 12") != std::string::npos,
                   "HEAD should keep would-be body length: " << resp);
    auto p = resp.find("\r\n\r\n");
    TEST_CHECK_MSG(p != std::string::npos && resp.substr(p + 4).empty(),
                   "HEAD must not send response body: " << resp);
    close(fd);
    server->stop();
    usleep(150 * 1000);
}

// 3. keep-alive:同一连接两次请求
static void test_keepalive() {
    bronx::BxIoManager iom(2);
    auto server = start_server(iom, GatewayOptions());
    usleep(150 * 1000);

    int fd = connect_fd();
    // 第一次(keep-alive)
    std::string r1 = raw_exchange(fd, "GET /one HTTP/1.1\r\nHost: x\r\n\r\n");
    TEST_CHECK_MSG(r1.find("/one|") != std::string::npos, "first req: " << r1.substr(0,40));
    TEST_CHECK_MSG(r1.find("keep-alive") != std::string::npos, "should keep-alive");
    // 第二次(同连接)
    std::string r2 = raw_exchange(fd, "GET /two HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    TEST_CHECK_MSG(r2.find("/two|") != std::string::npos, "second req on same conn: " << r2.substr(0,40));
    close(fd);
    server->stop();
    usleep(150 * 1000);
}

// 3b. 大 header 扩容后,连续低使用请求才触发连接级 InBuf 缩容。
static void test_keepalive_inputbuffer_trim_after_low_usage_rounds() {
    AllocStats stats;
    install_observed_allocator(&stats);
    {
        bronx::BxIoManager iom(2);
        GatewayOptions opts;
        opts.maxHeaderSize = 256 * 1024;
        auto server = start_server(iom, opts);
        usleep(150 * 1000);

        int fd = connect_fd();
        TEST_CHECK_MSG(fd >= 0, "connect");
        std::string bigHeader(70000, 'a');
        std::string wire =
            "GET /big HTTP/1.1\r\nHost: x\r\nX-Big: " + bigHeader + "\r\n\r\n";
        TEST_CHECK_MSG(send_all_fd(fd, wire), "send big request");

        std::string resp;
        TEST_CHECK_MSG(recv_until_contains(fd, "/big|", resp), "big request response");
        TEST_CHECK_MSG(stats.largeAllocs.load(std::memory_order_relaxed) > 0,
                       "large InBuf allocation should have happened");
        int freesAfterBig = stats.largeFrees.load(std::memory_order_relaxed);

        for(int i = 0; i < 2; ++i) {
            std::string req = "GET /small" + std::to_string(i)
                            + " HTTP/1.1\r\nHost: x\r\n\r\n";
            TEST_CHECK_MSG(send_all_fd(fd, req), "send small request " << i);
            TEST_CHECK_MSG(recv_until_contains(fd, "/small" + std::to_string(i) + "|", resp),
                           "small response " << i);
            TEST_CHECK_EQ(stats.largeFrees.load(std::memory_order_relaxed), freesAfterBig);
        }

        std::string resetHeader(9000, 'r');
        std::string resetReq =
            "GET /reset HTTP/1.1\r\nHost: x\r\nX-Reset: " + resetHeader + "\r\n\r\n";
        TEST_CHECK_MSG(send_all_fd(fd, resetReq), "send high-usage reset request");
        TEST_CHECK_MSG(recv_until_contains(fd, "/reset|", resp), "reset response");
        TEST_CHECK_EQ(stats.largeFrees.load(std::memory_order_relaxed), freesAfterBig);

        for(int i = 2; i < 5; ++i) {
            std::string req = "GET /small" + std::to_string(i)
                            + " HTTP/1.1\r\nHost: x\r\n\r\n";
            TEST_CHECK_MSG(send_all_fd(fd, req), "send small request " << i);
            TEST_CHECK_MSG(recv_until_contains(fd, "/small" + std::to_string(i) + "|", resp),
                           "small response " << i);
            TEST_CHECK_EQ(stats.largeFrees.load(std::memory_order_relaxed), freesAfterBig);
        }

        std::string fourthLow = "GET /small5 HTTP/1.1\r\nHost: x\r\n\r\n";
        TEST_CHECK_MSG(send_all_fd(fd, fourthLow), "send fourth low-usage request");
        TEST_CHECK_MSG(recv_until_contains(fd, "/small5|", resp), "fourth low response");
        TEST_CHECK_MSG(stats.largeFrees.load(std::memory_order_relaxed) > freesAfterBig,
                       "fourth low-usage round should trim large buffer");

        std::string after = "GET /after HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        TEST_CHECK_MSG(send_all_fd(fd, after), "send after-trim request");
        TEST_CHECK_MSG(recv_until_contains(fd, "/after|", resp),
                       "request after trim should still parse");

        close(fd);
        server->stop();
        usleep(150 * 1000);
    }
    reset_allocator();
}

// 3c. 粘包/预读下一条请求时,连接循环不能丢掉已在 InBuf 中的请求字节。
static void test_keepalive_prefetched_requests_survive() {
    bronx::BxIoManager iom(2);
    GatewayOptions opts;
    opts.maxHeaderSize = 256 * 1024;
    auto server = start_server(iom, opts);
    usleep(150 * 1000);

    int fd = connect_fd();
    TEST_CHECK_MSG(fd >= 0, "connect");
    std::string bigHeader(70000, 'p');
    std::string wire =
        "GET /prefetch-big HTTP/1.1\r\nHost: x\r\nX-Big: " + bigHeader + "\r\n\r\n";
    for(int i = 0; i < 6; ++i) {
        wire += "GET /prefetch" + std::to_string(i) + " HTTP/1.1\r\nHost: x\r\n\r\n";
    }
    wire += "GET /prefetch-close HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    TEST_CHECK_MSG(send_all_fd(fd, wire), "send pipelined requests");

    std::string resp;
    TEST_CHECK_MSG(recv_until_contains(fd, "/prefetch-big|", resp),
                   "prefetched big response");
    for(int i = 0; i < 6; ++i) {
        TEST_CHECK_MSG(recv_until_contains(fd, "/prefetch" + std::to_string(i) + "|", resp),
                       "prefetched response " << i);
    }
    TEST_CHECK_MSG(recv_until_contains(fd, "/prefetch-close|", resp),
                   "prefetched close response");

    close(fd);
    server->stop();
    usleep(150 * 1000);
}

// 4. body 超限 → 413
static void test_body_limit() {
    bronx::BxIoManager iom(2);
    GatewayOptions opts;
    opts.maxBodySize = 10;
    auto server = start_server(iom, opts);
    usleep(150 * 1000);

    int fd = connect_fd();
    std::string resp = raw_exchange(fd,
        "POST /big HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n");
    TEST_CHECK_MSG(resp.find("413") != std::string::npos, "should 413: " << resp.substr(0,30));
    close(fd);
    server->stop();
    usleep(150 * 1000);
}

// 4b. chunked body 也必须受 maxBodySize 限制
static void test_chunked_body_limit() {
    bronx::BxIoManager iom(2);
    GatewayOptions opts;
    opts.maxBodySize = 4;
    auto server = start_server(iom, opts);
    usleep(150 * 1000);

    int fd = connect_fd();
    std::string resp = raw_exchange(fd,
        "POST /big HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    TEST_CHECK_MSG(resp.find("413") != std::string::npos,
                   "oversized chunked body should be 413: " << resp.substr(0,40));
    close(fd);
    server->stop();
    usleep(150 * 1000);
}

// 5. drain:置 draining 后新连接不被处理,活跃连接收尾
static void test_drain() {
    bronx::BxIoManager iom(2);
    auto server = start_server(iom, GatewayOptions());
    usleep(150 * 1000);

    // 建一个连接并完成一次请求(keep-alive 保持)
    int fd = connect_fd();
    std::string r1 = raw_exchange(fd, "GET /x HTTP/1.1\r\nHost: x\r\n\r\n");
    TEST_CHECK_MSG(r1.find("200") != std::string::npos, "pre-drain ok");

    // drain:200ms 超时强关
    server->drain(200);
    TEST_CHECK_MSG(server->isDraining(), "draining flag set");
    TEST_CHECK_MSG(server->waitDrain(1000), "drain should finish");
    TEST_CHECK_EQ((int)server->getActiveConnectionCount(), 0);
    close(fd);
    server->stop();
    usleep(150 * 1000);
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_gw_connection start ===";
    test_basic_roundtrip();
    test_post_body();
    test_head_no_body();
    test_keepalive();
    test_keepalive_inputbuffer_trim_after_low_usage_rounds();
    test_keepalive_prefetched_requests_survive();
    test_body_limit();
    test_chunked_body_limit();
    test_drain();
    return TEST_SUMMARY();
}
