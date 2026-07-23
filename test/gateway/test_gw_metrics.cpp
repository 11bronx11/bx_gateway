#include "test_util.h"
#include "admin.h"
#include "conn.h"
#include "gateway.h"
#include "metrics.h"
#include "endpoint.h"
#include "reactor.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace bronx::gateway;

static uint16_t freePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if(::bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0
       || ::getsockname(fd, (sockaddr*)&addr, &len) != 0) {
        ::close(fd);
        return 0;
    }
    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

static int dial(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0) return fd;
    ::close(fd);
    return -1;
}

static std::string ask(uint16_t port, const std::string& wire) {
    int fd = dial(port);
    if(fd < 0) return {};
    size_t off = 0;
    while(off < wire.size()) {
        ssize_t n = ::send(fd, wire.data() + off, wire.size() - off, MSG_NOSIGNAL);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) break;
        off += n;
    }
    std::string out;
    char buf[4096];
    while(true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) break;
        out.append(buf, n);
    }
    ::close(fd);
    return out;
}

static void drop(uint16_t port) {
    int fd = dial(port);
    TEST_CHECK(fd >= 0);
    std::string req = "GET /drop HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    TEST_CHECK(::send(fd, req.data(), req.size(), MSG_NOSIGNAL) == (ssize_t)req.size());
    linger cut{1, 0};
    TEST_CHECK(::setsockopt(fd, SOL_SOCKET, SO_LINGER, &cut, sizeof(cut)) == 0);
    ::close(fd);
}

static uint64_t sum(const std::array<uint64_t, 10>& v) {
    uint64_t out = 0;
    for(uint64_t n : v) out += n;
    return out;
}

static uint64_t sum(const std::array<uint64_t, 12>& v) {
    uint64_t out = 0;
    for(uint64_t n : v) out += n;
    return out;
}

static void checkSnapshot() {
    GatewayMetrics metrics;
    std::atomic<size_t> left{4};
    std::vector<std::thread> workers;
    for(size_t i = 0; i < 4; ++i) {
        workers.emplace_back([&]() {
            for(size_t j = 0; j < 10000; ++j)
                metrics.finishReq(ReqStage::HANDLE, ReqResult::OK, 200, ReqCut::NONE, j);
            --left;
        });
    }
    while(left.load()) {
        auto snap = metrics.snapshot();
        TEST_CHECK_EQ(sum(snap.reqTime), snap.reqTimeCount);
    }
    for(auto& worker : workers) worker.join();
    auto snap = metrics.snapshot();
    TEST_CHECK_EQ(snap.requests, 40000u);
    TEST_CHECK_EQ(sum(snap.reqTime), snap.reqTimeCount);
}

int main() {
    GatewayMetrics local;
    local.decrConnections();
    local.wsClose();
    local.finishReq(static_cast<ReqStage>(2), ReqResult::OK, 200, ReqCut::NONE, 1);
    auto localSnap = local.snapshot();
    TEST_CHECK_EQ(localSnap.activeConns, 0);
    TEST_CHECK_EQ(localSnap.wsActive, 0);
    TEST_CHECK_EQ(localSnap.requests, 0u);
    TEST_CHECK_EQ(localSnap.resp2xx, 0u);
    checkSnapshot();

    uint16_t port = freePort();
    uint16_t adminPort = freePort();
    TEST_CHECK(port != 0 && adminPort != 0 && port != adminPort);

    GatewayOptions opts;
    opts.maxHeaderSize = 128;
    opts.maxBodySize = 4;
    bronx::BxIoManager iom(3, "gwmet");
    auto server = std::make_shared<GatewayServer>(opts, &iom, &iom);
    server->setRequestHandler([](GatewayConnection& conn) {
        std::string path = conn.request()->getPath();
        if(path == "/drop") usleep(200 * 1000);
        auto rsp = std::make_shared<GwResponse>();
        rsp->setStatus(path == "/one" ? 101 : path == "/move" ? 302 : 200);
        conn.sendResponse(rsp, "ok");
    });
    TEST_CHECK(server->bind(bronx::BxAddress::LookupAny(
        "127.0.0.1:" + std::to_string(port))));
    TEST_CHECK(server->start());
    auto admin = std::make_shared<AdminServer>(server.get(), &iom, &iom);
    TEST_CHECK(admin->bind(bronx::BxAddress::LookupAny(
        "127.0.0.1:" + std::to_string(adminPort))));
    TEST_CHECK(admin->start());
    usleep(100 * 1000);

    GatewayMetrics::instance().recordRoute("/tab\tvalue", 200);
    GatewayMetrics::instance().note_auth(0, 0, "/tab\tvalue");
    auto before = GatewayMetrics::instance().snapshot();
    TEST_CHECK(ask(port, "GET /one HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        .find("101 Switching Protocols") != std::string::npos);
    TEST_CHECK(ask(port, "GET /move HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        .find("302 Found") != std::string::npos);
    TEST_CHECK(ask(port, "NOPE\r\n\r\n").find("400 Bad Request") != std::string::npos);
    TEST_CHECK(ask(port, "POST /big HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\n")
        .find("413 Payload Too Large") != std::string::npos);
    std::string big(160, 'x');
    TEST_CHECK(ask(port, "GET /head HTTP/1.1\r\nHost: x\r\nX-Big: " + big + "\r\n\r\n")
        .find("431 Request Header Fields Too Large") != std::string::npos);
    drop(port);

    GatewayMetrics::Snapshot after;
    uint64_t due = bronx_test::now_ms() + 3000;
    do {
        after = GatewayMetrics::instance().snapshot();
        if(after.requests >= before.requests + 6) break;
        usleep(10 * 1000);
    } while(bronx_test::now_ms() < due);

    TEST_CHECK_EQ(after.requests - before.requests, 6u);
    TEST_CHECK_EQ(after.reqs[3] - before.reqs[3], 2u);
    TEST_CHECK_EQ(after.reqs[1] - before.reqs[1], 3u);
    TEST_CHECK_EQ(after.reqs[5] - before.reqs[5], 1u);
    TEST_CHECK_EQ(after.resp1xx - before.resp1xx, 1u);
    TEST_CHECK_EQ(after.resp3xx - before.resp3xx, 1u);
    TEST_CHECK_EQ(after.resp4xx - before.resp4xx, 3u);
    TEST_CHECK_EQ(after.responses[3] - before.responses[3], 1u);
    TEST_CHECK_EQ(after.rejects[(size_t)ReqCut::BAD_HEADER]
                  - before.rejects[(size_t)ReqCut::BAD_HEADER], 1u);
    TEST_CHECK_EQ(after.rejects[(size_t)ReqCut::BODY_TOO_LARGE]
                  - before.rejects[(size_t)ReqCut::BODY_TOO_LARGE], 1u);
    TEST_CHECK_EQ(after.rejects[(size_t)ReqCut::HEADER_TOO_LARGE]
                  - before.rejects[(size_t)ReqCut::HEADER_TOO_LARGE], 1u);
    TEST_CHECK_EQ(after.clientWriteErrors - before.clientWriteErrors, 1u);
    TEST_CHECK_EQ(after.reqTimeCount - before.reqTimeCount, 6u);
    TEST_CHECK_EQ(sum(after.responses) - sum(before.responses), 6u);
    TEST_CHECK_EQ(sum(after.reqTime), after.reqTimeCount);

    std::string met = ask(adminPort,
        "GET /metrics HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    TEST_CHECK(met.find("gateway_requests_total{stage=\"header\",result=\"rejected\"}") != std::string::npos);
    TEST_CHECK(met.find("gateway_responses_total{class=\"1xx\",result=\"sent\"}") != std::string::npos);
    TEST_CHECK(met.find("gateway_request_rejects_total{reason=\"headers_too_large\"}") != std::string::npos);
    TEST_CHECK(met.find("gateway_client_write_errors_total " + std::to_string(after.clientWriteErrors)) != std::string::npos);
    TEST_CHECK(met.find("gateway_request_seconds_bucket{le=\"+Inf\"} "
                        + std::to_string(after.reqTimeCount)) != std::string::npos);
    TEST_CHECK(met.find("gateway_request_seconds_sum ") != std::string::npos);
    TEST_CHECK(met.find("gateway_request_seconds_count "
                        + std::to_string(after.reqTimeCount)) != std::string::npos);
    TEST_CHECK(met.find("gateway_route_requests_total{route=\"/tab\tvalue\"} 1")
               != std::string::npos);
    TEST_CHECK(met.find("gateway_auth_total{scheme=\"jwt\",result=\"ok\",route=\"/tab\tvalue\"} 1")
               != std::string::npos);

    std::string stats = ask(adminPort,
        "GET /stats HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    TEST_CHECK(stats.find("\"requests\":" + std::to_string(after.requests)) != std::string::npos);
    TEST_CHECK(stats.find("\"request_seconds_count\":"
                          + std::to_string(after.reqTimeCount)) != std::string::npos);

    admin->stop();
    server->stop();
    usleep(100 * 1000);
    iom.stop();
    return TEST_SUMMARY();
}
