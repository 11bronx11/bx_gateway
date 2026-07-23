// API 网关核心链路测试
// =====================
// 覆盖真实 socket 链路下的路由、JWT 鉴权、per-route 限流、负载均衡。
// 测试目标不是压测性能，而是证明请求经过 GatewayConnection → MwChain
// → Router → Proxy → UpstreamGroup 后，命中的路由与上游选择符合预期。

#include "test_util.h"
#include "gateway.h"
#include "conn.h"
#include "ctx.h"
#include "mw.h"
#include "middlewares/builtin.h"
#include "middlewares/proxy.h"
#include "middlewares/stubs.h"
#include "jwt.h"
#include "router.h"
#include "ups_group.h"
#include "lb.h"
#include "reactor.h"
#include "endpoint.h"
#include <jwt-cpp/jwt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace bronx::gateway;

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

static uint16_t GW_PORT = 19100;
static uint16_t UP_A_PORT = 19101;
static uint16_t UP_B_PORT = 19102;
static uint16_t UP_C_PORT = 19103;

static void choosePorts() {
    // 测试可能被并行执行；端口按 pid 分片，避免多个进程互相抢固定端口
    // 后把请求打进别的测试实例，污染限流桶和 LB 计数。
    uint16_t base = 19100 + (getpid() % 100) * 4;
    GW_PORT = base;
    UP_A_PORT = base + 1;
    UP_B_PORT = base + 2;
    UP_C_PORT = base + 3;
}

struct MockEndpoint {
    std::string id;
    uint16_t port = 0;
    uint32_t delayMs = 0;
    std::atomic<int> hits{0};
    GatewayServer::ptr server;
};

static int connectFd(uint16_t port, const std::string& sourceIp = "") {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return -1;
    if(!sourceIp.empty()) {
        sockaddr_in src{};
        src.sin_family = AF_INET;
        src.sin_port = 0;
        inet_pton(AF_INET, sourceIp.c_str(), &src.sin_addr);
        if(bind(fd, (sockaddr*)&src, sizeof(src)) != 0) {
            close(fd);
            return -1;
        }
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if(connect(fd, (sockaddr*)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static std::string recvAll(int fd) {
    std::string out;
    char buf[4096];
    int n;
    while((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        out.append(buf, n);
    }
    return out;
}

static std::string requestRaw(uint16_t port, const std::string& raw,
                              const std::string& sourceIp = "") {
    int fd = connectFd(port, sourceIp);
    if(fd < 0) return "";
    send(fd, raw.data(), raw.size(), 0);
    std::string resp = recvAll(fd);
    close(fd);
    return resp;
}

static int statusOf(const std::string& resp) {
    auto sp1 = resp.find(' ');
    if(sp1 == std::string::npos) return 0;
    auto sp2 = resp.find(' ', sp1 + 1);
    if(sp2 == std::string::npos) return 0;
    return std::stoi(resp.substr(sp1 + 1, sp2 - sp1 - 1));
}

static std::string bodyOf(const std::string& resp) {
    auto p = resp.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : resp.substr(p + 4);
}

static std::string tokenFor(const std::string& secret,
                            const std::string& issuer,
                            const std::string& sub) {
    return jwt::create()
        .set_type("JWT")
        .set_issuer(issuer)
        .set_payload_claim("sub", jwt::claim(sub))
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::hs256{secret});
}

static std::string noneToken(const std::string& issuer, const std::string& sub) {
    return jwt::create()
        .set_type("JWT")
        .set_issuer(issuer)
        .set_payload_claim("sub", jwt::claim(sub))
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::none{});
}

static std::string makeReq(const std::string& method,
                           const std::string& path,
                           const std::string& host,
                           const std::string& token = "",
                           const std::string& apiKey = "",
                           const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "Connection: close\r\n";
    if(!token.empty()) {
        req += "Authorization: Bearer " + token + "\r\n";
    }
    if(!apiKey.empty()) {
        req += "X-API-Key: " + apiKey + "\r\n";
    }
    for(const auto& h : headers) {
        req += h.first + ": " + h.second + "\r\n";
    }
    req += "\r\n";
    return req;
}

static MockEndpoint* addMock(std::vector<std::unique_ptr<MockEndpoint>>& mocks,
                             bronx::BxIoManager& iom,
                             const std::string& id,
                             uint16_t port,
                             uint32_t delayMs = 0) {
    auto ep = std::make_unique<MockEndpoint>();
    ep->id = id;
    ep->port = port;
    ep->delayMs = delayMs;
    auto raw = ep.get();
    ep->server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    ep->server->setRequestHandler([raw](GatewayConnection& c) {
        raw->hits.fetch_add(1);
        if(raw->delayMs) {
            usleep(raw->delayMs * 1000);
        }
        auto reader = c.requestBody();
        if(reader) {
            std::string sink;
            while(reader->readChunk(sink) > 0) sink.clear();
        }
        auto rsp = std::make_shared<GwResponse>();
        rsp->setStatus(200);
        rsp->setHeader("Content-Type", "text/plain");
        std::string body = "upstream=" + raw->id
            + ";method=" + HttpMethodToString(c.request()->getMethod())
            + ";path=" + c.request()->getPath()
            + ";host=" + c.request()->getHeader("Host");
        c.sendResponse(rsp, body);
    });
    TEST_CHECK_MSG(ep->server->bind(bronx::BxAddress::LookupAny(
                       "127.0.0.1:" + std::to_string(port))),
                   "bind mock upstream " << id << ":" << port);
    ep->server->start();
    mocks.push_back(std::move(ep));
    return raw;
}

static UpstreamGroup::ptr groupOf(const std::string& name,
                                  const std::string& lb,
                                  const std::vector<std::pair<MockEndpoint*, uint32_t>>& eps) {
    auto g = std::make_shared<UpstreamGroup>(name, MakeLoadBalancer(lb));
    for(auto& e : eps) {
        g->addEndpoint(std::make_shared<Endpoint>(
            "127.0.0.1", e.first->port, e.second,
            CircuitBreakerConfig{}, ConnPoolConfig{}));
    }
    return g;
}

static void addRoute(const Router::ptr& router,
                     const std::string& name,
                     MatchType type,
                     const std::string& path,
                     const std::string& upstream,
                     const std::vector<std::string>& methods = {},
                     const std::string& host = "",
                     int priority = 0) {
    RouteRule r;
    r.name = name;
    r.matchType = type;
    r.pathPattern = path;
    r.upstream = upstream;
    r.methods = methods;
    r.host = host;
    r.priority = priority;
    router->addRoute(r);
}

static void addAuthRoute(const Router::ptr& router,
                         const std::string& name,
                         const std::string& path,
                         const std::string& authPolicy) {
    RouteRule r;
    r.name = name;
    r.matchType = MatchType::EXACT;
    r.pathPattern = path;
    r.upstream = "single-a";
    r.methods = {"GET"};
    r.authPolicy = authPolicy;
    router->addRoute(r);
}

static void addLimitedRoute(const Router::ptr& router) {
    RouteRule r;
    r.name = "limited";
    r.matchType = MatchType::EXACT;
    r.pathPattern = "/limited";
    r.upstream = "single-a";
    r.methods = {"GET"};
    r.rateLimitEnabled = true;
    r.rateLimitCapacity = 2;
    r.rateLimitRefillPerSec = 0;
    r.rateLimitKey = "user";
    router->addRoute(r);
}

static void addLimitedIpRoute(const Router::ptr& router) {
    RouteRule r;
    r.name = "limited-ip";
    r.matchType = MatchType::EXACT;
    r.pathPattern = "/limited-ip";
    r.upstream = "single-a";
    r.methods = {"GET"};
    r.authPolicy = "none";
    r.rateLimitEnabled = true;
    r.rateLimitCapacity = 1;
    r.rateLimitRefillPerSec = 0;
    r.rateLimitKey = "ip";
    router->addRoute(r);
}

static void addLimitedPublicUserRoute(const Router::ptr& router) {
    RouteRule r;
    r.name = "limited-public-user";
    r.matchType = MatchType::EXACT;
    r.pathPattern = "/limited-public-user";
    r.upstream = "single-a";
    r.methods = {"GET"};
    r.authPolicy = "none";
    r.rateLimitEnabled = true;
    r.rateLimitCapacity = 1;
    r.rateLimitRefillPerSec = 0;
    r.rateLimitKey = "user";
    router->addRoute(r);
}

static MwChain::ptr makeCoreChain(const Router::ptr& router,
                                          const UpstreamRegistry::ptr& upstreams,
                                          const std::string& secret,
                                          const std::string& issuer) {
    auto chain = std::make_shared<MwChain>();
    JwtAuthConfig cfg;
    cfg.enabled = true;
    cfg.secret = secret;
    cfg.issuer = issuer;
    chain->use(MakeRouterMiddleware(router, upstreams));
    RouteAuthConfig authCfg;
    authCfg.globalJwtEnabled = true;
    authCfg.apiKeys = {"core-key"};
    chain->use(MakeRouteAuthMiddleware(std::make_shared<JwtAuthenticator>(cfg), authCfg));
    chain->use(MakePerRouteRateLimitMiddleware());
    chain->use(MakeProxyMiddleware(2000, 5000));
    return chain;
}

static void testRoutingAndAuth(const std::string& token,
                               const std::string& badToken,
                               const std::string& algNoneToken) {
    // 精确路由 + method + host：Host 域名大小写与端口不应影响匹配。
    {
        std::string resp = requestRaw(GW_PORT, makeReq("GET", "/exact", "API.TEST:19100", token));
        TEST_CHECK_EQ(statusOf(resp), 200);
        TEST_CHECK_MSG(bodyOf(resp).find("upstream=A") != std::string::npos, bodyOf(resp));
        TEST_CHECK_MSG(bodyOf(resp).find("path=/exact") != std::string::npos, bodyOf(resp));
    }
    // 同一路径按 method 分流，不能错误回退到 GET 路由。
    {
        std::string resp = requestRaw(GW_PORT, makeReq("POST", "/exact", "api.test", token));
        TEST_CHECK_EQ(statusOf(resp), 200);
        TEST_CHECK_MSG(bodyOf(resp).find("upstream=B") != std::string::npos, bodyOf(resp));
    }
    // 最长前缀优先：/api/admin 优先于 /api。
    {
        std::string resp = requestRaw(GW_PORT, makeReq("GET", "/api/admin/users", "api.test", token));
        TEST_CHECK_EQ(statusOf(resp), 200);
        TEST_CHECK_MSG(bodyOf(resp).find("upstream=B") != std::string::npos, bodyOf(resp));
        TEST_CHECK_MSG(bodyOf(resp).find("path=/api/admin/users") != std::string::npos, bodyOf(resp));
    }
    // Host 维度优先：host 专属规则优先于通配规则。
    {
        std::string resp = requestRaw(GW_PORT, makeReq("GET", "/tenant", "api.test", token));
        TEST_CHECK_EQ(statusOf(resp), 200);
        TEST_CHECK_MSG(bodyOf(resp).find("upstream=A") != std::string::npos, bodyOf(resp));
        resp = requestRaw(GW_PORT, makeReq("GET", "/tenant", "other.test", token));
        TEST_CHECK_EQ(statusOf(resp), 200);
        TEST_CHECK_MSG(bodyOf(resp).find("upstream=C") != std::string::npos, bodyOf(resp));
    }
    // JWT：缺失、错签、alg=none 都必须被拒绝。
    {
        TEST_CHECK_EQ(statusOf(requestRaw(GW_PORT, makeReq("GET", "/api/ping", "api.test"))), 401);
        TEST_CHECK_EQ(statusOf(requestRaw(GW_PORT, makeReq("GET", "/api/ping", "api.test", badToken))), 401);
        TEST_CHECK_EQ(statusOf(requestRaw(GW_PORT, makeReq("GET", "/api/ping", "api.test", algNoneToken))), 401);
    }
    // Per-route auth：none 公开；api_key 只接受配置 key；jwt 显式走 JWT。
    {
        TEST_CHECK_EQ(statusOf(requestRaw(GW_PORT, makeReq("GET", "/public", "api.test"))), 200);
        TEST_CHECK_EQ(statusOf(requestRaw(GW_PORT, makeReq("GET", "/apikey", "api.test"))), 401);
        TEST_CHECK_EQ(statusOf(requestRaw(GW_PORT, makeReq("GET", "/apikey", "api.test", "", "bad"))), 401);
        TEST_CHECK_EQ(statusOf(requestRaw(GW_PORT, makeReq("GET", "/apikey", "api.test", "", "core-key"))), 200);
        TEST_CHECK_EQ(statusOf(requestRaw(GW_PORT, makeReq("GET", "/jwt-only", "api.test"))), 401);
        TEST_CHECK_EQ(statusOf(requestRaw(GW_PORT, makeReq("GET", "/jwt-only", "api.test", token))), 200);
    }
}

static void testRateLimit(const std::string& tokenA, const std::string& tokenB) {
    std::string r1 = requestRaw(GW_PORT, makeReq("GET", "/limited", "api.test", tokenA));
    std::string r2 = requestRaw(GW_PORT, makeReq("GET", "/limited", "api.test", tokenA));
    std::string r3 = requestRaw(GW_PORT, makeReq("GET", "/limited", "api.test", tokenA));
    TEST_CHECK_EQ(statusOf(r1), 200);
    TEST_CHECK_EQ(statusOf(r2), 200);
    TEST_CHECK_EQ(statusOf(r3), 429);

    // key=user 时，不同 JWT sub 必须使用不同桶。
    std::string rb = requestRaw(GW_PORT, makeReq("GET", "/limited", "api.test", tokenB));
    TEST_CHECK_EQ(statusOf(rb), 200);

    // key=ip 是安全决策,没有 trusted proxy 配置时必须按 TCP peer 限流,
    // 不能信任客户端可伪造的 X-Forwarded-For。
    std::string ip1 = requestRaw(GW_PORT, makeReq("GET", "/limited-ip", "api.test", "", "", {
        {"X-Forwarded-For", "10.0.0.1, 192.168.0.1"}
    }));
    std::string ip2 = requestRaw(GW_PORT, makeReq("GET", "/limited-ip", "api.test", "", "", {
        {"X-Forwarded-For", "10.0.0.2, 192.168.0.2"}
    }));
    TEST_CHECK_EQ(statusOf(ip1), 200);
    TEST_CHECK_EQ(statusOf(ip2), 429);

    // key=user 但没有 JWT sub 时,退回到 TCP peer,避免所有匿名请求共享空 key。
    std::string anon1 = requestRaw(GW_PORT, makeReq("GET", "/limited-public-user", "api.test", "", "", {
        {"X-Forwarded-For", "10.0.0.11"}
    }), "127.0.0.2");
    std::string anon2 = requestRaw(GW_PORT, makeReq("GET", "/limited-public-user", "api.test", "", "", {
        {"X-Forwarded-For", "10.0.0.22"}
    }), "127.0.0.3");
    std::string anon3 = requestRaw(GW_PORT, makeReq("GET", "/limited-public-user", "api.test", "", "", {
        {"X-Forwarded-For", "10.0.0.11"}
    }), "127.0.0.2");
    TEST_CHECK_EQ(statusOf(anon1), 200);
    TEST_CHECK_EQ(statusOf(anon2), 200);
    TEST_CHECK_EQ(statusOf(anon3), 429);
}

static void testRoundRobin() {
    std::vector<std::string> seen;
    for(int i = 0; i < 6; ++i) {
        std::string resp = requestRaw(GW_PORT, makeReq("GET", "/rr", "api.test",
            tokenFor("gw-secret", "gw-test", "rr-user")));
        TEST_CHECK_EQ(statusOf(resp), 200);
        seen.push_back(bodyOf(resp));
    }
    int a = 0, b = 0, c = 0;
    for(auto& body : seen) {
        if(body.find("upstream=A") != std::string::npos) ++a;
        if(body.find("upstream=B") != std::string::npos) ++b;
        if(body.find("upstream=C") != std::string::npos) ++c;
    }
    TEST_CHECK_EQ(a, 2);
    TEST_CHECK_EQ(b, 2);
    TEST_CHECK_EQ(c, 2);
}

static void testWeighted() {
    int a = 0, b = 0, c = 0;
    for(int i = 0; i < 20; ++i) {
        std::string resp = requestRaw(GW_PORT, makeReq("GET", "/weighted", "api.test",
            tokenFor("gw-secret", "gw-test", "weighted-user")));
        TEST_CHECK_EQ(statusOf(resp), 200);
        std::string body = bodyOf(resp);
        if(body.find("upstream=A") != std::string::npos) ++a;
        if(body.find("upstream=B") != std::string::npos) ++b;
        if(body.find("upstream=C") != std::string::npos) ++c;
    }
    TEST_CHECK_MSG(a > b && b >= c && c > 0,
                   "weighted full path should prefer 5:3:2 order, got A="
                   << a << " B=" << b << " C=" << c);
}

static void testLeastConn() {
    std::vector<std::thread> ts;
    std::vector<std::string> bodies(6);
    for(size_t i = 0; i < bodies.size(); ++i) {
        ts.emplace_back([i, &bodies]() {
            std::string resp = requestRaw(GW_PORT, makeReq("GET", "/least", "api.test",
                tokenFor("gw-secret", "gw-test", "least-user-" + std::to_string(i))));
            TEST_CHECK_EQ(statusOf(resp), 200);
            bodies[i] = bodyOf(resp);
        });
    }
    for(auto& t : ts) t.join();

    int a = 0, b = 0, c = 0;
    for(auto& body : bodies) {
        if(body.find("upstream=A") != std::string::npos) ++a;
        if(body.find("upstream=B") != std::string::npos) ++b;
        if(body.find("upstream=C") != std::string::npos) ++c;
    }
    TEST_CHECK_MSG(a > 0 && b > 0 && c > 0,
                   "least_conn should spread concurrent requests, got A="
                   << a << " B=" << b << " C=" << c);
}

static void testLoadBalancerUnits() {
    TEST_CHECK(IsSupportedLoadBalancer("round_robin"));
    TEST_CHECK(IsSupportedLoadBalancer("weighted"));
    TEST_CHECK(IsSupportedLoadBalancer("least_conn"));
    TEST_CHECK(IsSupportedLoadBalancer("weighted_least_conn"));
    TEST_CHECK(!IsSupportedLoadBalancer("consistent_hash"));

    auto fallback = MakeLoadBalancer("unknown");
    std::vector<LbCand> fb = {{0, 1, 0}, {1, 1, 0}};
    TEST_CHECK_EQ(fallback->select(fb), (size_t)0);
    TEST_CHECK_EQ(fallback->select(fb), (size_t)1);

    WeightedRr weighted;
    std::vector<LbCand> wc = {{0, 5, 0}, {1, 3, 0}, {2, 2, 0}};
    int wa = 0, wb = 0, wc2 = 0;
    for(int i = 0; i < 10; ++i) {
        size_t idx = weighted.select(wc);
        if(idx == 0) ++wa;
        if(idx == 1) ++wb;
        if(idx == 2) ++wc2;
    }
    TEST_CHECK_EQ(wa, 5);
    TEST_CHECK_EQ(wb, 3);
    TEST_CHECK_EQ(wc2, 2);

    WeightedRr zeroWeighted;
    std::vector<LbCand> zc = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    TEST_CHECK_EQ(zeroWeighted.select(zc), (size_t)0);
    TEST_CHECK_EQ(zeroWeighted.select(zc), (size_t)1);
    TEST_CHECK_EQ(zeroWeighted.select(zc), (size_t)2);

    WeightedRr mixedZeroWeighted;
    std::vector<LbCand> mzc = {{0, 0, 0}, {1, 4, 0}, {2, 0, 0}};
    for(int i = 0; i < 6; ++i) {
        TEST_CHECK_EQ(mixedZeroWeighted.select(mzc), (size_t)1);
    }

    WeightedRr hugeWeighted;
    std::vector<LbCand> hc = {
        {0, kMaxLoadBalancerWeight, 0},
        {1, kMaxLoadBalancerWeight, 0},
        {2, kMaxLoadBalancerWeight, 0}
    };
    TEST_CHECK_EQ(hugeWeighted.select(hc), (size_t)0);
    TEST_CHECK_EQ(hugeWeighted.select(hc), (size_t)1);
    TEST_CHECK_EQ(hugeWeighted.select(hc), (size_t)2);

    WeightedRr changingWeighted;
    std::vector<LbCand> cc = {{0, 5, 0}, {1, 1, 0}};
    TEST_CHECK_EQ(changingWeighted.select(cc), (size_t)0);
    cc = {{0, 1, 0}, {1, 5, 0}};
    TEST_CHECK_EQ(changingWeighted.select(cc), (size_t)1);

    LeastConn least;
    std::vector<LbCand> lc = {{0, 1, 4}, {1, 1, 1}, {2, 1, 3}};
    TEST_CHECK_EQ(least.select(lc), (size_t)1);
    lc = {{0, 1, 0}, {1, 1, 0}, {2, 1, 0}};
    size_t l0 = least.select(lc);
    size_t l1 = least.select(lc);
    size_t l2 = least.select(lc);
    TEST_CHECK_MSG(l0 != l1 && l1 != l2 && l0 != l2,
                   "least_conn tie-break should rotate equal candidates");

    WeightLeast weightLeast;
    std::vector<LbCand> wlc = {{0, 1, 4}, {1, 2, 6}, {2, 1, 8}};
    TEST_CHECK_EQ(weightLeast.select(wlc), (size_t)1);
    wlc = {{0, 1, 1}, {1, 2, 2}, {2, 3, 3}};
    size_t w0 = weightLeast.select(wlc);
    size_t w1 = weightLeast.select(wlc);
    size_t w2 = weightLeast.select(wlc);
    TEST_CHECK_MSG(w0 != w1 && w1 != w2 && w0 != w2,
                   "weighted_least_conn tie-break should rotate equal ratios");
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    choosePorts();
    BRONX_LOG_INFO(g_logger) << "=== test_gw_core start ===";

    const std::string secret = "gw-secret";
    const std::string issuer = "gw-test";
    const std::string tokenA = tokenFor(secret, issuer, "user-a");
    const std::string tokenB = tokenFor(secret, issuer, "user-b");
    const std::string badToken = tokenFor("bad-secret", issuer, "user-a");
    const std::string algNone = noneToken(issuer, "user-a");

    bronx::BxIoManager iom(8);
    std::vector<std::unique_ptr<MockEndpoint>> mocks;
    auto a = addMock(mocks, iom, "A", UP_A_PORT, 250);
    auto b = addMock(mocks, iom, "B", UP_B_PORT, 40);
    auto c = addMock(mocks, iom, "C", UP_C_PORT, 0);

    auto upstreams = std::make_shared<UpstreamRegistry>();
    upstreams->add(groupOf("single-a", "round_robin", {{a, 1}}));
    upstreams->add(groupOf("single-b", "round_robin", {{b, 1}}));
    upstreams->add(groupOf("single-c", "round_robin", {{c, 1}}));
    upstreams->add(groupOf("rr", "round_robin", {{a, 1}, {b, 1}, {c, 1}}));
    upstreams->add(groupOf("weighted", "weighted", {{a, 5}, {b, 3}, {c, 2}}));
    upstreams->add(groupOf("least", "least_conn", {{a, 1}, {b, 1}, {c, 1}}));

    auto router = std::make_shared<Router>();
    addRoute(router, "exact-get", MatchType::EXACT, "/exact", "single-a", {"GET"}, "api.test");
    addRoute(router, "exact-post", MatchType::EXACT, "/exact", "single-b", {"POST"}, "api.test");
    addRoute(router, "api-admin", MatchType::PREFIX, "/api/admin", "single-b", {"GET"});
    addRoute(router, "api", MatchType::PREFIX, "/api", "single-c", {"GET"});
    addRoute(router, "tenant-host", MatchType::EXACT, "/tenant", "single-a", {"GET"}, "api.test");
    addRoute(router, "tenant-any", MatchType::EXACT, "/tenant", "single-c", {"GET"});
    addLimitedRoute(router);
    addLimitedIpRoute(router);
    addLimitedPublicUserRoute(router);
    addRoute(router, "rr", MatchType::EXACT, "/rr", "rr", {"GET"});
    addRoute(router, "weighted", MatchType::EXACT, "/weighted", "weighted", {"GET"});
    addRoute(router, "least", MatchType::EXACT, "/least", "least", {"GET"});
    addAuthRoute(router, "public", "/public", "none");
    addAuthRoute(router, "apikey", "/apikey", "api_key");
    addAuthRoute(router, "jwt-only", "/jwt-only", "jwt");

    auto chain = makeCoreChain(router, upstreams, secret, issuer);
    auto gw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    gw->setRequestHandler([chain](GatewayConnection& c) {
        ReqCtx ctx(&c);
        chain->run(ctx);
    });
    TEST_CHECK_MSG(gw->bind(bronx::BxAddress::LookupAny(
                       "127.0.0.1:" + std::to_string(GW_PORT))),
                   "bind gateway core test");
    gw->start();
    usleep(250 * 1000);

    testRoutingAndAuth(tokenA, badToken, algNone);
    testRateLimit(tokenA, tokenB);
    testLoadBalancerUnits();
    testRoundRobin();
    testWeighted();
    testLeastConn();

    gw->stop();
    for(auto& m : mocks) {
        m->server->stop();
    }
    usleep(250 * 1000);
    return TEST_SUMMARY();
}
