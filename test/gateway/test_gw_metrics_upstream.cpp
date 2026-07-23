// 关键路径补测：per-endpoint 上游/熔断指标导出 (admin.cpp::write_up_metrics)
// 全测试原本零覆盖的导出块：gateway_upstream_requests_total / _inflight /
// _latency_ms / _rejected_total / gateway_circuit_state / _transitions_total。
// 手动喂 ep->stat / ep->cb，构造 ConfigSnapshot 给 GatewayServer，
// 从 admin /metrics 抓取，断言导出行确实出现且数值正确。不依赖真实上游。
#include "test_util.h"
#include "admin.h"
#include "metrics.h"
#include "gateway.h"
#include "ups_group.h"
#include "up_ret.h"
#include "lb.h"
#include "reactor.h"
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

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

static std::string ask(uint16_t port, const std::string& wire) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { ::close(fd); return {}; }
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

int main() {
    uint16_t port = freePort();
    uint16_t adminPort = freePort();
    TEST_CHECK(port != 0 && adminPort != 0 && port != adminPort);

    bronx::BxIoManager iom(2, "gwup");
    GatewayOptions opts;
    auto server = std::make_shared<GatewayServer>(opts, &iom, &iom);
    TEST_CHECK(server->bind(bronx::BxAddress::LookupAny(
        "127.0.0.1:" + std::to_string(port))));
    TEST_CHECK(server->start());

    // 构造带一个 endpoint 的上游组，手动喂统计数据（不连真实后端）
    auto reg = std::make_shared<UpstreamRegistry>();
    auto group = std::make_shared<UpstreamGroup>("backend", MakeLoadBalancer("round_robin"));
    CircuitBreakerConfig cbCfg;
    ConnPoolConfig poolCfg;
    auto ep = std::make_shared<Endpoint>("127.0.0.1", 9, 1, cbCfg, poolCfg, 0);
    group->addEndpoint(ep);
    reg->add(group);

    // 喂 per-endpoint stat：3 次 OK(带延迟)、1 次 FAIL/CONNECT、1 次 reject(BUSY)
    { UpRet r; r.mark = UpMark::OK;   r.why = UpWhy::NONE;    r.costMs = 7;   ep->stat.add(r); }
    { UpRet r; r.mark = UpMark::OK;   r.why = UpWhy::NONE;    r.costMs = 42;  ep->stat.add(r); }
    { UpRet r; r.mark = UpMark::OK;   r.why = UpWhy::NONE;    r.costMs = 120; ep->stat.add(r); }
    { UpRet r; r.mark = UpMark::FAIL; r.why = UpWhy::CONNECT; r.costMs = 3;   ep->stat.add(r); }
    ep->stat.reject(UpWhy::BUSY);
    // 造一次熔断状态迁移 CLOSED->OPEN（recordFailure 累计到阈值）
    for(int i = 0; i < (int)cbCfg.failureThreshold; ++i) ep->cb->recordFailure();
    ep->activeConns.store(2);

    // 全局上游计数（recordUp）——upstreamOk 的成功路径原本零断言
    auto& M = GatewayMetrics::instance();
    auto g0 = M.snapshot();
    { UpRet r; r.mark = UpMark::OK; M.recordUp(r); }
    { UpRet r; r.mark = UpMark::OK; M.recordUp(r); }
    { UpRet r; r.mark = UpMark::FAIL; M.recordUp(r); }
    auto g1 = M.snapshot();
    TEST_CHECK_EQ(g1.upstreamOk - g0.upstreamOk, 2u);
    TEST_CHECK_EQ(g1.upstreamFail - g0.upstreamFail, 1u);

    // 装配 config 快照给 server（router/chain 留空，导出块只读 upstreams）
    auto cfg = std::make_shared<ConfigSnapshot>();
    cfg->upstreams = reg;
    server->setConfig(cfg);

    auto admin = std::make_shared<AdminServer>(server.get(), &iom, &iom);
    TEST_CHECK(admin->bind(bronx::BxAddress::LookupAny(
        "127.0.0.1:" + std::to_string(adminPort))));
    TEST_CHECK(admin->start());
    usleep(100 * 1000);

    std::string met = ask(adminPort,
        "GET /metrics HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    TEST_CHECK(!met.empty());

    // ---- per-endpoint 上游导出（write_up_metrics，零覆盖块）----
    // requests_total{result=ok,reason=none} == 3
    TEST_CHECK_MSG(met.find(
        "gateway_upstream_requests_total{upstream=\"backend\",endpoint=\"127.0.0.1:9\",result=\"ok\",reason=\"none\"} 3")
        != std::string::npos, "upstream ok requests export");
    // requests_total{result=fail,reason=connect} == 1
    TEST_CHECK_MSG(met.find(
        "gateway_upstream_requests_total{upstream=\"backend\",endpoint=\"127.0.0.1:9\",result=\"fail\",reason=\"connect\"} 1")
        != std::string::npos, "upstream fail requests export");
    // inflight gauge == 2
    TEST_CHECK_MSG(met.find(
        "gateway_upstream_inflight{upstream=\"backend\",endpoint=\"127.0.0.1:9\"} 2")
        != std::string::npos, "upstream inflight export");
    // rejected_total{reason=busy} == 1
    TEST_CHECK_MSG(met.find(
        "gateway_upstream_rejected_total{upstream=\"backend\",endpoint=\"127.0.0.1:9\",reason=\"busy\"} 1")
        != std::string::npos, "upstream rejected export");
    // latency 直方图：+Inf 桶累计 == 4 次带延迟样本 (3 OK + 1 FAIL)
    TEST_CHECK_MSG(met.find(
        "gateway_upstream_latency_ms{upstream=\"backend\",endpoint=\"127.0.0.1:9\",bucket=\"+Inf\"} 4")
        != std::string::npos, "upstream latency +Inf bucket export");
    // circuit_state gauge == 1 (OPEN)
    TEST_CHECK_MSG(met.find(
        "gateway_circuit_state{upstream=\"backend\",endpoint=\"127.0.0.1:9\"} 1")
        != std::string::npos, "circuit state OPEN export");
    // transitions_total{closed->open} == 1
    TEST_CHECK_MSG(met.find(
        "gateway_circuit_transitions_total{upstream=\"backend\",endpoint=\"127.0.0.1:9\",from=\"closed\",to=\"open\"} 1")
        != std::string::npos, "circuit transition closed->open export");

    // ---- 全局上游导出 ----
    TEST_CHECK_MSG(met.find("gateway_upstream_total{result=\"ok\"} " + std::to_string(g1.upstreamOk))
        != std::string::npos, "global upstream ok export");
    TEST_CHECK_MSG(met.find("gateway_upstream_total{result=\"fail\"} " + std::to_string(g1.upstreamFail))
        != std::string::npos, "global upstream fail export");

    admin->stop();
    server->stop();
    usleep(100 * 1000);
    iom.stop();
    return TEST_SUMMARY();
}
