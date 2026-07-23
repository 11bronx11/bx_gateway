// test_gateway_v2.cpp — Round 2 单元测试
// 覆盖：Router / CircuitBreaker / ConnectionPool / JWT / RateLimit / IPFilter / Metrics
// 不需要真实 socket；需要 jwt-cpp 用于 JWT 签名测试。

#include "router.h"
#include "ronduan.h"
#include "conn_pool.h"
#include "metrics.h"
#include "mempool.h"
#include "ups_group.h"
#include "lb.h"
#include "conf.h"
#include "gateway.h"
#include "middlewares/builtin.h"
#include "middlewares/proxy.h"
#include "middlewares/stubs.h"
#include "http_msg.h"
#include "jwt.h"
#include "log.h"
#include <jwt-cpp/jwt.h>
#include <openssl/sha.h>
#include <yaml-cpp/yaml.h>
#include <cassert>
#include <thread>
#include <vector>
#include <iostream>
#include <atomic>
#include <fstream>
#include <cstdio>
#include <limits>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace bronx::gateway;

#define CHECK(cond) do { if(!(cond)) { std::cerr << "FAIL: " #cond " (" << __FILE__ << ":" << __LINE__ << ")\n"; std::exit(1); } } while(0)

static UpstreamRegistry registryWithGroup(const std::string& name) {
    UpstreamRegistry reg;
    auto group = std::make_shared<UpstreamGroup>(name, MakeLoadBalancer("round_robin"));
    group->addEndpoint(std::make_shared<Endpoint>(
        "127.0.0.1", 9, 1, CircuitBreakerConfig{}, ConnPoolConfig{}));
    reg.add(group);
    return reg;
}

struct OneShotListener {
    int fd = -1;
    uint16_t port = 0;
    std::thread worker;
    int accepts = 1;
    bool sendStaleByte = false;

    explicit OneShotListener(int acceptCount = 1, bool staleByte = false)
        : accepts(acceptCount)
        , sendStaleByte(staleByte) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fd >= 0);
        int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        CHECK(::bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
        CHECK(::listen(fd, 4) == 0);
        socklen_t len = sizeof(addr);
        CHECK(::getsockname(fd, (sockaddr*)&addr, &len) == 0);
        port = ntohs(addr.sin_port);
        worker = std::thread([this]() {
            for(int i = 0; i < accepts; ++i) {
                int c = ::accept(fd, nullptr, nullptr);
                if(c >= 0) {
                    if(sendStaleByte) {
                        const char byte = 'x';
                        ::send(c, &byte, 1, 0);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    ::close(c);
                } else {
                    break;
                }
            }
        });
    }

    ~OneShotListener() {
        if(fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if(worker.joinable()) worker.join();
    }
};

// ─── Router ───────────────────────────────────────────────────────────────────

static void testRouterExact() {
    auto table = std::make_shared<RouteTable>();
    RouteRule r; r.name = "exact"; r.matchType = MatchType::EXACT;
    r.pathPattern = "/ping"; r.upstream = "u";
    table->addRule(r); table->build();
    auto match = table->match("GET", "", "/ping");
    CHECK(match != nullptr && match->name == "exact");
    CHECK(table->match("GET", "", "/ping/extra") == nullptr);
    CHECK(table->match("GET", "", "/pin")        == nullptr);
    std::cout << "PASS: routerExact\n";
}

static void testRouterPrefix() {
    auto table = std::make_shared<RouteTable>();
    RouteRule r1; r1.name = "short"; r1.matchType = MatchType::PREFIX;
    r1.pathPattern = "/api"; r1.upstream = "u";
    RouteRule r2; r2.name = "long";  r2.matchType = MatchType::PREFIX;
    r2.pathPattern = "/api/v2"; r2.upstream = "u";
    table->addRule(r1); table->addRule(r2); table->build();
    // 最长前缀优先
    auto m1 = table->match("GET", "", "/api/v2/foo");
    CHECK(m1 && m1->name == "long");
    auto m2 = table->match("GET", "", "/api/v1/foo");
    CHECK(m2 && m2->name == "short");
    // 不跨段
    CHECK(table->match("GET", "", "/apifoo") == nullptr);
    std::cout << "PASS: routerPrefix\n";
}

static void testRouterMethodFilter() {
    auto table = std::make_shared<RouteTable>();
    RouteRule r; r.name = "post-only"; r.matchType = MatchType::EXACT;
    r.pathPattern = "/submit"; r.methods = {"POST"}; r.upstream = "u";
    table->addRule(r); table->build();
    CHECK(table->match("POST", "", "/submit") != nullptr);
    CHECK(table->match("GET",  "", "/submit") == nullptr);
    std::cout << "PASS: routerMethodFilter\n";
}

static void testRouterExtendedMethodRaw() {
    auto table = std::make_shared<RouteTable>();
    RouteRule r;
    r.name = "webdav";
    r.matchType = MatchType::EXACT;
    r.pathPattern = "/resource";
    r.methods = {"PROPFIND"};
    r.upstream = "u";
    table->addRule(r);
    table->build();

    Router router;
    router.setTable(table);
    auto req = std::make_shared<GwRequest>();
    req->setMethod(HttpMethod::INVALID_METHOD);
    req->setMethodRaw("PROPFIND");
    req->setPath("/resource");
    ReqCtx ctx(req);
    auto reg = registryWithGroup("u");
    CHECK(router.match(ctx, reg));
    CHECK(ctx.route().rule && ctx.route().rule->name == "webdav");
    std::cout << "PASS: routerExtendedMethodRaw\n";
}

static void testRouterTieBreak() {
    auto table = std::make_shared<RouteTable>();
    RouteRule a; a.name = "nohost"; a.matchType = MatchType::EXACT;
    a.pathPattern = "/x"; a.upstream = "u"; a.host = "";
    RouteRule b; b.name = "withhost"; b.matchType = MatchType::EXACT;
    b.pathPattern = "/x"; b.upstream = "u"; b.host = "api.example.com";
    table->addRule(a); table->addRule(b); table->build();
    auto m = table->match("GET", "api.example.com", "/x");
    CHECK(m && m->name == "withhost");
    auto m2 = table->match("GET", "other.com", "/x");
    CHECK(m2 && m2->name == "nohost");
    std::cout << "PASS: routerTieBreak\n";
}

static void testRouterGenerationCacheInvalidation() {
    // generation 变化 → cache key 不同 → 旧缓存自然 miss
    Router r;
    uint32_t g0 = r.generation();
    RouteRule rule; rule.name = "r1"; rule.matchType = MatchType::PREFIX;
    rule.pathPattern = "/v1"; rule.upstream = "u";
    r.addRoute(rule); // addRoute 触发 generation +1
    CHECK(r.generation() > g0);
    std::cout << "PASS: routerGenerationCacheInvalidation\n";
}

static void testRouterDefaultArcCache() {
    Router router;
    RouteRule rule;
    rule.name = "cached";
    rule.matchType = MatchType::EXACT;
    rule.pathPattern = "/cached";
    rule.upstream = "u";
    router.addRoute(rule);

    auto req = std::make_shared<GwRequest>();
    req->setMethod(HttpMethod::GET);
    req->setPath("/cached");
    auto reg = registryWithGroup("u");

    ReqCtx first(req);
    CHECK(router.match(first, reg));
    ReqCtx second(req);
    CHECK(router.match(second, reg));

    uint64_t hits = 0, misses = 0;
    router.cacheStats(hits, misses);
    CHECK(hits == 1 && misses == 1);
    std::cout << "PASS: routerDefaultArcCache\n";
}

static void testRouterListRoutes() {
    Router r;
    RouteRule rule; rule.name = "listed"; rule.matchType = MatchType::PREFIX;
    rule.pathPattern = "/listed"; rule.upstream = "u"; rule.authPolicy = "api_key";
    r.addRoute(rule);
    auto routes = r.listRoutes();
    CHECK(routes.size() == 1);
    CHECK(routes[0].name == "listed");
    CHECK(routes[0].authPolicy == "api_key");
    std::cout << "PASS: routerListRoutes\n";
}

static void testRouterSetTableThenAddRoutePreservesExistingRules() {
    auto table = std::make_shared<RouteTable>();
    RouteRule base;
    base.name = "base";
    base.matchType = MatchType::EXACT;
    base.pathPattern = "/base";
    base.upstream = "u";
    table->addRule(base);
    table->build();

    Router router;
    router.setTable(table);

    RouteRule extra;
    extra.name = "extra";
    extra.matchType = MatchType::EXACT;
    extra.pathPattern = "/extra";
    extra.upstream = "u";
    router.addRoute(extra);

    auto routes = router.listRoutes();
    CHECK(routes.size() == 2);

    auto reg = registryWithGroup("u");
    auto reqBase = std::make_shared<GwRequest>();
    reqBase->setMethod(HttpMethod::GET);
    reqBase->setPath("/base");
    ReqCtx ctxBase(reqBase);
    CHECK(router.match(ctxBase, reg));
    CHECK(ctxBase.route().rule && ctxBase.route().rule->name == "base");

    auto reqExtra = std::make_shared<GwRequest>();
    reqExtra->setMethod(HttpMethod::GET);
    reqExtra->setPath("/extra");
    ReqCtx ctxExtra(reqExtra);
    CHECK(router.match(ctxExtra, reg));
    CHECK(ctxExtra.route().rule && ctxExtra.route().rule->name == "extra");
    std::cout << "PASS: routerSetTableThenAddRoutePreservesExistingRules\n";
}

struct SimpleRouteCache : public RouteCache {
    bool get(std::string_view key, RouteResult& out) override {
        lastGetKey = std::string(key);
        auto it = values.find(lastGetKey);
        if(it == values.end()) return false;
        out = it->second;
        return true;
    }

    void put(std::string_view key, const RouteResult& result) override {
        lastPutKey = std::string(key);
        values[lastPutKey] = result;
    }

    std::map<std::string, RouteResult> values;
    std::string lastGetKey;
    std::string lastPutKey;
};

static void testRouterKeyExtractorAndNullCache() {
    Router router;
    router.setCache(nullptr);
    RouteRule rule;
    rule.name = "model-route";
    rule.pathPattern = "gpt-4o";
    rule.upstream = "u";
    router.addRoute(rule);
    router.setKeyExtractor([](const ReqCtx& ctx) {
        std::string model;
        return ctx.getAttr("model", model) ? model : std::string();
    });

    auto req = std::make_shared<GwRequest>();
    req->setMethod(HttpMethod::POST);
    req->setMethodRaw("POST");
    req->setPath("/v1/chat/completions");
    req->setHeader("Host", "API.EXAMPLE.COM:443");
    ReqCtx ctx(req);
    ctx.setAttr("model", std::string("gpt-4o"));
    auto reg = registryWithGroup("u");

    CHECK(router.match(ctx, reg));
    CHECK(ctx.route().rule && ctx.route().rule->name == "model-route");

    auto cache = std::make_shared<SimpleRouteCache>();
    router.setCache(cache);
    ReqCtx ctx2(req);
    ctx2.setAttr("model", std::string("gpt-4o"));
    CHECK(router.match(ctx2, reg));
    CHECK(cache->lastPutKey.find("|POST|api.example.com|gpt-4o") != std::string::npos);

    std::string oldKey = cache->lastPutKey;
    router.setKeyExtractor(nullptr);
    ReqCtx ctx3(req);
    CHECK(!router.match(ctx3, reg));
    CHECK(cache->lastPutKey != oldKey);
    CHECK(cache->lastPutKey.find("|POST|api.example.com|/v1/chat/completions") != std::string::npos);
    std::cout << "PASS: routerKeyExtractorAndNullCache\n";
}

class RejectAuthenticator : public Authenticator {
public:
    AuthErr authenticate(ReqCtx&) override { return AuthErr::NoToken; }
};

class FixedAuth : public Authenticator {
public:
    explicit FixedAuth(AuthErr err) : m_err(err) {}
    AuthErr authenticate(ReqCtx&) override { return m_err; }
private:
    AuthErr m_err;
};

static uint64_t authCount(const GatewayMetrics::Snapshot& snap, const std::string& route,
                          size_t scheme, size_t result) {
    for(const auto& row : snap.authByRoute) {
        if(row.first == route) return row.second[scheme * 12 + result];
    }
    return 0;
}

static void testAuthMetrics() {
    RouteAuthConfig cfg;
    cfg.globalJwtEnabled = true;
    cfg.apiKeys = {"good"};
    const std::vector<std::pair<AuthErr, size_t>> jwt = {
        {AuthErr::Ok, 0}, {AuthErr::NoToken, 1}, {AuthErr::BadToken, 2},
        {AuthErr::BadSig, 4}, {AuthErr::Expired, 5}, {AuthErr::BadIss, 6},
        {AuthErr::BadAud, 7}, {AuthErr::BadAlg, 8}
    };
    for(const auto& one : jwt) {
        auto req = std::make_shared<GwRequest>();
        ReqCtx ctx(req);
        RouteRule rule;
        rule.name = "auth-metrics";
        rule.authPolicy = "jwt";
        ctx.route().rule = std::make_shared<RouteRule>(rule);
        bool next = false;
        auto before = GatewayMetrics::instance().snapshot();
        MakeRouteAuthMiddleware(std::make_shared<FixedAuth>(one.first), cfg)->handle(
            ctx, [&]() { next = true; });
        auto after = GatewayMetrics::instance().snapshot();
        CHECK(authCount(after, rule.name, 0, one.second)
              == authCount(before, rule.name, 0, one.second) + 1);
        CHECK(next == (one.first == AuthErr::Ok));
        if(one.first != AuthErr::Ok) CHECK(ctx.response()->getStatus() == 401);
        std::string scheme, result;
        CHECK(ctx.getAttr("auth", scheme) && scheme == "jwt");
        CHECK(ctx.getAttr("auth_result", result));
    }

    for(bool role : {false, true}) {
        auto req = std::make_shared<GwRequest>();
        ReqCtx ctx(req);
        RouteRule rule;
        rule.name = "auth-metrics";
        rule.authPolicy = "jwt";
        if(role) rule.reqRoles = {"ops"};
        else rule.reqScopes = {"read"};
        ctx.route().rule = std::make_shared<RouteRule>(rule);
        size_t result = role ? 10 : 9;
        auto before = GatewayMetrics::instance().snapshot();
        MakeRouteAuthMiddleware(std::make_shared<FixedAuth>(AuthErr::Ok), cfg)->handle(ctx, []() {});
        auto after = GatewayMetrics::instance().snapshot();
        CHECK(authCount(after, rule.name, 0, result)
              == authCount(before, rule.name, 0, result) + 1);
        CHECK(ctx.response()->getStatus() == 403);
    }

    for(const std::pair<std::string, size_t>& one : {
            std::pair<std::string, size_t>{"", 1}, {"wrong", 3}, {"good", 0}}) {
        auto req = std::make_shared<GwRequest>();
        if(!one.first.empty()) req->setHeader("X-API-Key", one.first);
        ReqCtx ctx(req);
        RouteRule rule;
        rule.name = "auth-metrics";
        rule.authPolicy = "api_key";
        ctx.route().rule = std::make_shared<RouteRule>(rule);
        bool next = false;
        auto before = GatewayMetrics::instance().snapshot();
        MakeRouteAuthMiddleware(nullptr, cfg)->handle(ctx, [&]() { next = true; });
        auto after = GatewayMetrics::instance().snapshot();
        CHECK(authCount(after, rule.name, 1, one.second)
              == authCount(before, rule.name, 1, one.second) + 1);
        CHECK(next == (one.first == "good"));
        if(!next) CHECK(ctx.response()->getStatus() == 401);
    }

    auto req = std::make_shared<GwRequest>();
    ReqCtx ctx(req);
    RouteRule rule;
    rule.name = "auth-metrics";
    rule.authPolicy = "odd";
    ctx.route().rule = std::make_shared<RouteRule>(rule);
    auto before = GatewayMetrics::instance().snapshot();
    MakeRouteAuthMiddleware(nullptr, cfg)->handle(ctx, []() {});
    auto after = GatewayMetrics::instance().snapshot();
    CHECK(authCount(after, rule.name, 2, 11)
          == authCount(before, rule.name, 2, 11) + 1);
    CHECK(ctx.response()->getStatus() == 403);
    std::cout << "PASS: authMetrics\n";
}

static void testMiddlewareShortCircuitWithoutConnection() {
    auto req = std::make_shared<GwRequest>();
    req->setMethod(HttpMethod::GET);
    req->setPath("/missing-upstream");

    auto router = std::make_shared<Router>();
    RouteRule route;
    route.name = "missing-upstream";
    route.matchType = MatchType::EXACT;
    route.pathPattern = "/missing-upstream";
    route.upstream = "not_registered";
    router->addRoute(route);
    auto upstreams = std::make_shared<UpstreamRegistry>();
    auto routerChain = std::make_shared<MwChain>();
    routerChain->use(MakeRouterMiddleware(router, upstreams));
    routerChain->use([](ReqCtx&, const NextFn&) {
        CHECK(false);
    }, "must_not_run");
    ReqCtx routerCtx(req);
    routerChain->run(routerCtx);
    CHECK(routerCtx.isHandled());
    CHECK(routerCtx.response() != nullptr);
    CHECK(routerCtx.response()->getStatus() == 503);

    IPFilterConfig ipCfg;
    ipCfg.enabled = true;
    ipCfg.mode = "allowlist";
    ipCfg.cidrs = {"127.0.0.1/32"};
    ReqCtx ipCtx(req);
    MakeIPFilterMiddleware(ipCfg)->handle(ipCtx, []() {
        CHECK(false);
    });
    CHECK(ipCtx.isHandled());
    CHECK(ipCtx.response() != nullptr);
    CHECK(ipCtx.response()->getStatus() == 403);

    ReqCtx authCtx(req);
    MakeAuthMiddleware(std::make_shared<RejectAuthenticator>())->handle(authCtx, []() {
        CHECK(false);
    });
    CHECK(authCtx.isHandled());
    CHECK(authCtx.response() != nullptr);
    CHECK(authCtx.response()->getStatus() == 401);

    auto wsReq = std::make_shared<GwRequest>();
    wsReq->setMethod(HttpMethod::GET);
    wsReq->setPath("/ws");
    wsReq->setWebsocket(true);
    ReqCtx wsCtx(wsReq);
    MakeWebSocketTunnelStub()->handle(wsCtx, []() {
        CHECK(false);
    });
    CHECK(wsCtx.isHandled());
    CHECK(wsCtx.response() != nullptr);
    CHECK(wsCtx.response()->getStatus() == 503);
    std::cout << "PASS: middlewareShortCircuitWithoutConnection\n";
}

// ─── CircuitBreaker ───────────────────────────────────────────────────────────

static void testCircuitBreakerClosed() {
    CircuitBreakerConfig cfg; cfg.failureThreshold = 3;
    CircuitBreaker cb(cfg);
    CHECK(cb.isAllowed());
    cb.recordFailure(); cb.recordFailure();
    CHECK(cb.isAllowed()); // still CLOSED
    cb.recordFailure(); // → OPEN
    CHECK(!cb.isAllowed());
    std::cout << "PASS: circuitBreakerClosed→Open\n";
}

static void testCircuitBreakerHalfOpen() {
    CircuitBreakerConfig cfg; cfg.failureThreshold = 1; cfg.openTimeoutMs = 1;
    CircuitBreaker cb(cfg);
    cb.recordFailure();
    CHECK(!cb.isAllowed());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(cb.isAllowed()); // HALF_OPEN probe
    cb.recordSuccess();
    CHECK(cb.state() == CircuitBreaker::State::CLOSED);
    std::cout << "PASS: circuitBreakerHalfOpen\n";
}

static void testCircuitBreakerHalfOpenFail() {
    CircuitBreakerConfig cfg; cfg.failureThreshold = 1; cfg.openTimeoutMs = 1;
    CircuitBreaker cb(cfg);
    cb.recordFailure();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(cb.isAllowed()); // probe
    cb.recordFailure(); // → OPEN again
    CHECK(cb.state() == CircuitBreaker::State::OPEN);
    CHECK(!cb.isAllowed());
    std::cout << "PASS: circuitBreakerHalfOpenFail\n";
}

static void testCircuitBreakerHalfOpenMaxRequests() {
    CircuitBreakerConfig cfg; cfg.failureThreshold = 1;
    cfg.openTimeoutMs = 1; cfg.halfOpenMaxRequests = 1;
    CircuitBreaker cb(cfg);
    cb.recordFailure();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(cb.isAllowed());  // probe 1
    CHECK(!cb.isAllowed()); // 超过 halfOpenMaxRequests
    std::cout << "PASS: circuitBreakerHalfOpenMaxRequests\n";
}

static void testCircuitBreakerOpenIgnoresStaleSuccess() {
    CircuitBreakerConfig cfg;
    cfg.failureThreshold = 1;
    cfg.openTimeoutMs = 1;
    cfg.halfOpenMaxRequests = 1;
    CircuitBreaker cb(cfg);
    cb.recordFailure();
    CHECK(cb.state() == CircuitBreaker::State::OPEN);
    cb.recordSuccess();
    CHECK(cb.state() == CircuitBreaker::State::OPEN);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(cb.isAllowed());
    cb.recordSuccess();
    CHECK(cb.state() == CircuitBreaker::State::CLOSED);
    std::cout << "PASS: circuitBreakerOpenIgnoresStaleSuccess\n";
}

static void testCircuitBreakerConfigClamp() {
    CircuitBreakerConfig cfg;
    cfg.failureThreshold = 0;
    cfg.openTimeoutMs = 0;
    cfg.halfOpenMaxRequests = 0;
    CircuitBreaker cb(cfg);
    cb.recordFailure();
    CHECK(cb.state() == CircuitBreaker::State::OPEN);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(cb.isAllowed());
    CHECK(!cb.isAllowed());
    std::cout << "PASS: circuitBreakerConfigClamp\n";
}

static void testUpRetSkipAndWindow() {
    CircuitBreakerConfig cfg;
    cfg.failureThreshold = 2;
    cfg.failureRate = 0;
    cfg.slowRate = 0;
    CircuitBreaker cb(cfg);
    cb.record(UpRet{UpMark::FAIL, UpWhy::CONNECT});
    cb.record(UpRet{UpMark::SKIP, UpWhy::BAD_BODY});
    CHECK(cb.state() == CircuitBreaker::State::CLOSED);
    cb.record(UpRet{UpMark::FAIL, UpWhy::SEND});
    CHECK(cb.state() == CircuitBreaker::State::OPEN);

    cfg.failureThreshold = 100;
    cfg.windowMs = 20;
    cfg.buckets = 2;
    cfg.minRequests = 4;
    cfg.failureRate = 50;
    CircuitBreaker rate(cfg);
    rate.record(UpRet{UpMark::FAIL, UpWhy::CONNECT});
    rate.record(UpRet{UpMark::FAIL, UpWhy::CONNECT});
    rate.record(UpRet{UpMark::OK, UpWhy::NONE});
    CHECK(rate.state() == CircuitBreaker::State::CLOSED);
    rate.record(UpRet{UpMark::OK, UpWhy::NONE});
    CHECK(rate.state() == CircuitBreaker::State::OPEN);

    cfg.minRequests = 2;
    CircuitBreaker expired(cfg);
    expired.record(UpRet{UpMark::FAIL, UpWhy::CONNECT});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    expired.record(UpRet{UpMark::OK, UpWhy::NONE});
    expired.record(UpRet{UpMark::OK, UpWhy::NONE});
    CHECK(expired.state() == CircuitBreaker::State::CLOSED);
    std::cout << "PASS: upRetSkipAndWindow\n";
}

static void testCircuitSlowAndHalfOpenTurn() {
    CircuitBreakerConfig slowCfg;
    slowCfg.failureThreshold = 100;
    slowCfg.minRequests = 2;
    slowCfg.failureRate = 0;
    slowCfg.slowRate = 50;
    CircuitBreaker slow(slowCfg);
    slow.record(UpRet{UpMark::OK, UpWhy::NONE, 200, 2, true});
    slow.record(UpRet{UpMark::OK, UpWhy::NONE});
    CHECK(slow.state() == CircuitBreaker::State::OPEN);

    CircuitBreakerConfig cfg;
    cfg.failureThreshold = 1;
    cfg.openTimeoutMs = 2;
    cfg.maxOpenTimeoutMs = 8;
    cfg.halfOpenMaxRequests = 3;
    cfg.halfOpenSuccesses = 2;
    CircuitBreaker cb(cfg);
    cb.recordFailure();
    CHECK(cb.snapshot().openMs == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    uint64_t a = 0, b = 0;
    CHECK(cb.isAllowed(&a));
    CHECK(cb.isAllowed(&b));
    cb.record(UpRet{UpMark::OK, UpWhy::NONE}, a);
    CHECK(cb.state() == CircuitBreaker::State::HALF_OPEN);
    cb.record(UpRet{UpMark::FAIL, UpWhy::STATUS}, b);
    CHECK(cb.state() == CircuitBreaker::State::OPEN);
    CHECK(cb.snapshot().openMs == 4);
    cb.record(UpRet{UpMark::OK, UpWhy::NONE}, a);
    CHECK(cb.state() == CircuitBreaker::State::OPEN);
    std::this_thread::sleep_for(std::chrono::milliseconds(6));
    uint64_t c = 0;
    CHECK(cb.isAllowed(&c));
    cb.record(UpRet{UpMark::FAIL, UpWhy::TIMEOUT}, c);
    CHECK(cb.snapshot().openMs == 8);
    std::cout << "PASS: circuitSlowAndHalfOpenTurn\n";
}

static void testEndpointHoldAndUpStat() {
    Endpoint ep("127.0.0.1", 9, 1, CircuitBreakerConfig{}, ConnPoolConfig{}, 2);
    CHECK(ep.tryHold());
    CHECK(ep.tryHold());
    CHECK(!ep.tryHold());
    ep.dropHold();
    CHECK(ep.tryHold());
    ep.dropHold();
    ep.dropHold();
    CHECK(ep.activeConns.load() == 0);

    ep.stat.add(UpRet{UpMark::OK, UpWhy::NONE, 200, 7});
    ep.stat.add(UpRet{UpMark::FAIL, UpWhy::STATUS, 503, 11});
    ep.stat.add(UpRet{UpMark::SKIP, UpWhy::BAD_BODY});
    ep.stat.reject(UpWhy::BUSY);
    CHECK(ep.stat.requests(UpMark::OK, UpWhy::NONE) == 1);
    CHECK(ep.stat.requests(UpMark::FAIL, UpWhy::STATUS) == 1);
    CHECK(ep.stat.requests(UpMark::SKIP, UpWhy::BAD_BODY) == 1);
    CHECK(ep.stat.rejects(UpWhy::BUSY) == 1);
    CHECK(ep.stat.latencyCount() == 2);
    std::cout << "PASS: endpointHoldAndUpStat\n";
}

static void testWeightLeast() {
    CHECK(IsSupportedLoadBalancer("weighted_least_conn"));
    WeightLeast lb;
    std::vector<LbCand> c = {{0, 1, 4}, {1, 2, 6}, {2, 1, 7}};
    CHECK(lb.select(c) == 1);
    c = {{0, 1, 1}, {1, 2, 2}, {2, 3, 3}};
    size_t a = lb.select(c);
    size_t b = lb.select(c);
    size_t d = lb.select(c);
    CHECK(a != b && b != d && a != d);
    std::cout << "PASS: weightLeast\n";
}

// ─── Metrics ──────────────────────────────────────────────────────────────────

static void testMetricsConcurrent() {
    // 重置（每轮测试用独立 Metrics 局部对象，而非 singleton，以免干扰）
    // 改用 incrRequests 写并发：N 线程各加 1000 次，总应为 N*1000
    struct LocalMetrics {
        std::atomic<uint64_t> requests{0};
        void incr() { ++requests; }
    } m;
    constexpr int N = 8, OPS = 1000;
    std::vector<std::thread> ts;
    for(int i = 0; i < N; ++i) ts.emplace_back([&m]{ for(int j=0; j<OPS; ++j) m.incr(); });
    for(auto& t : ts) t.join();
    CHECK(m.requests.load() == N * OPS);
    std::cout << "PASS: metricsConcurrent\n";
}

static void testMetricsLatencyPercentiles() {
    GatewayMetrics m;
    m.incr_acq_fail();
    m.incr_no_healthy();
    m.recordLatency(250);
    auto s = m.snapshot();
    CHECK(s.upstreamAcquireFail == 1);
    CHECK(s.noHealthyEndpoint == 1);
    CHECK(s.latCount == 1);
    CHECK(s.latP50Ms == 250);
    CHECK(s.latP99Ms == 250);
    m.recordLatency(1);
    m.recordLatency(5000);
    s = m.snapshot();
    CHECK(s.latP50Ms == 250);
    CHECK(s.latP99Ms == 5000);
    std::cout << "PASS: metricsLatencyPercentiles\n";
}

// ─── TokenBucket（Per-route RateLimit 核心）─────────────────────────────────

static void testTokenBucket() {
    TokenBucket bucket(5, 100); // 容量5，快速补充
    bool ok = true;
    for(int i = 0; i < 5 && ok; ++i) ok = bucket.tryAcquire();
    CHECK(ok);
    CHECK(!bucket.tryAcquire()); // 第6次拒绝
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 补5个token
    CHECK(bucket.tryAcquire()); // 补充后放行
    std::cout << "PASS: tokenBucket\n";
}

// ─── IPFilter ─────────────────────────────────────────────────────────────────

static void testIPFilter() {
    // denylist
    IPFilterConfig deny;
    deny.enabled = true; deny.mode = "denylist";
    deny.cidrs = {"1.2.3.4", "127.0.0.0/8"};
    // allowlist
    IPFilterConfig allow;
    allow.enabled = true; allow.mode = "allowlist";
    allow.cidrs = {"1.2.3.4"};
    // 验证中间件不 panic（功能验证通过 middleware 调用，这里仅验证接口构建）
    auto mw_deny  = MakeIPFilterMiddleware(deny);
    auto mw_allow = MakeIPFilterMiddleware(allow);
    CHECK(mw_deny  != nullptr);
    CHECK(mw_allow != nullptr);
    std::cout << "PASS: ipFilterBuild\n";
}

static std::string makeJwt(const std::string& secret) {
    return jwt::create()
        .set_type("JWT")
        .set_payload_claim("sub", jwt::claim(std::string("alice")))
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::hs256{secret});
}

static void testJwtEmptySecretFailsClosed() {
    JwtAuthConfig cfg;
    cfg.enabled = true;
    cfg.secret = "";
    JwtAuthenticator auth(cfg);

    auto req = std::make_shared<GwRequest>();
    req->setMethod(HttpMethod::GET);
    req->setMethodRaw("GET");
    req->setPath("/secure");
    req->setHeader("Authorization", "Bearer " + makeJwt(""));
    ReqCtx ctx(req);

    CHECK(auth.authenticate(ctx) != AuthErr::Ok);
    std::cout << "PASS: jwtEmptySecretFailsClosed\n";
}

// 测试用固定密钥对, 私钥签发 公钥验
static const char* kTestRsaPriv = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCmrAUoLWToZXo2
QoBS7ArR7khCqGk8URZS/fOXX5ax1Ume+VK+Dl3Up43UzKfxPVPTVyuUEFz/QaYC
OnDde0XL+mtIdRfqPKnQKCSp6YyBW3qd/G+BFubW5H+PjpNaPpZhlHr3btXixOpQ
GwdB//tsmqLHZ+bdZPI82NDRZBLcRDUV3VmNjAv7+CALjMuWUacKq4nFzDUSQRZC
vH2536VApE/+rS8KcpoR2gxf3cQ2xYYnaTwQhSWvXPONUPRuXDfXR9NA9YWpaM+V
pJk3aBVIIbkGbdgFrkPuHzzLkjdahdxIO1xlMSH2MQSPY2vh0aokuddHWvGZygMl
CXls02G7AgMBAAECggEAL7DcHuVyUL8sq5b2wgNxiyVL/urSheRL535r853x8Duv
/7Gmt+RDfARfpIrU6UXblQcF3K2b9pwRxOR1BCLwU0/537dFmww1qO67ovTFdAQ5
fzhKrZzge4/RYkHxLurmu9AxKVjJEcS3qSk6t+tnxWkv96/m0uW+HKQLiEbC0Myq
Yxi6zM3Ugdi5sbu1qvhAFay8fHYhtcwlfdfUr/1FERZQ5r9B5/fD9+ULCkxwvynH
gJsTtrIC1kEhUhOIjZsmXB+PzK/M+UU29Z5FP/ZGsrNc7NuBPIh1E1ynAq0lPfow
ve44kg8hRYCU5FGnju2AciwlOXJCJOA5GuN4c+jaCQKBgQDqD5YvqpydggPYWKZ5
Ljt5G0o1OVy5NieMXM2GetJFcRG1do84XHVhpF4OBNI7YLeAskkum9MmB6aOqRmd
mqxF602EToY+S6KEHNg+SZT5pQ0XQOSwRplknRAuYrZPi6RXaY2d2I7SZA7ZqB7S
2+Y7HNGO9cP4uo+K7jlduo1NiQKBgQC2S2acfY6vk/pHg91eK/dr/TKuTDlW7vKY
AdUvO83DpzMuAhWWF5TNBa9WenNcSHQ/mBQtnzW6fjEdBS/CxHvKNK/R6M8adyuw
M6uyHHi0mIG0rssVfmsif4oCqHIUgGK6RB3kOUSw6UCqoNiTMxr4UmAHNOF+mbI8
DOUZfImIIwKBgQCcRJnF60ezFTnR+KX0o/xLCABMdqbEdTaETPVfAqNef3YOTECk
pX4tGZs+CtP9lr4NK3sWpiyb6GG4P4f9pGW+LLJNUkvoTYEMaWGqfF/9KMgLrWTB
l/ETrpU+EuObSiUHw04sg4gfSXRstwUVTIFzF/OFWepkJtJtNE2SmVl72QKBgQCX
aTVHUbpL91rwP4SEjXwqg9muj70oibuRqiQJ4WBc2+lUk4KeceO0Dr6902MyCTQV
BhnscsrLpbjhY8dxvIUdRHmsnfQ3BTiSV7iKbb+MMQe6rVwc9C3EL67P2l02zhQy
phU11KNLVdHSZ1q7mL0T+EeudKu+3aX+3q8DZj9NhQKBgBox7Uc2ZEZUaBHDQf2z
WWByoG6IErqSRMjuo8OHeHoKIfnpQ+T7VZGvw6NWrNKOOHowLOUkHDLZ1ATE7Cqo
VaSjYxqMQT9J8ABUllEdDnWZsvqp0IjzwprH0AzpYS9wk/wGNfB+7IZvtWw6f6sR
V76KwftOgIIbK6Vdmil0oOOF
-----END PRIVATE KEY-----)";

static const char* kTestRsaPub = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEApqwFKC1k6GV6NkKAUuwK
0e5IQqhpPFEWUv3zl1+WsdVJnvlSvg5d1KeN1Myn8T1T01crlBBc/0GmAjpw3XtF
y/prSHUX6jyp0CgkqemMgVt6nfxvgRbm1uR/j46TWj6WYZR6927V4sTqUBsHQf/7
bJqix2fm3WTyPNjQ0WQS3EQ1Fd1ZjYwL+/ggC4zLllGnCquJxcw1EkEWQrx9ud+l
QKRP/q0vCnKaEdoMX93ENsWGJ2k8EIUlr1zzjVD0blw310fTQPWFqWjPlaSZN2gV
SCG5Bm3YBa5D7h88y5I3WoXcSDtcZTEh9jEEj2Nr4dGqJLnXR1rxmcoDJQl5bNNh
uwIDAQAB
-----END PUBLIC KEY-----)";

static const char* kTestEcPub = R"(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEBcinlpAXWpzKdIjVHDq0po4+FxY9
YVDEL/aQRtQVZJQ++/9ujZsZ4TYZd6PI+5PCs8hQnvAuRExaOmKUK6qUOg==
-----END PUBLIC KEY-----)";

static const char* kTestEcPriv = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgh6i9VrdeF4smWofC
pGnQD55fUU9FD4kLTEnoB2G73kChRANCAAQFyKeWkBdanMp0iNUcOrSmjj4XFj1h
UMQv9pBG1BVklD77/26NmxnhNhl3o8j7k8KzyFCe8C5ETFo6YpQrqpQ6
-----END PRIVATE KEY-----)";

static ReqCtx makeCtxWithBearer(const std::string& token,
                                std::shared_ptr<GwRequest>& holder) {
    holder = std::make_shared<GwRequest>();
    holder->setMethod(HttpMethod::GET);
    holder->setMethodRaw("GET");
    holder->setPath("/secure");
    holder->setHeader("Authorization", "Bearer " + token);
    return ReqCtx(holder);
}

// RS256 公钥验签放行, 抽 sub/scope/roles
static void testJwtRs256VerifyAndClaims() {
    std::string token = jwt::create()
        .set_type("JWT")
        .set_issuer("bronx")
        .set_audience("gw")
        .set_payload_claim("sub", jwt::claim(std::string("bob")))
        .set_payload_claim("scope", jwt::claim(std::string("read write")))
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::rs256{kTestRsaPub, kTestRsaPriv, "", ""});

    JwtAuthConfig cfg;
    cfg.enabled = true; cfg.algo = "RS256"; cfg.pubKey = kTestRsaPub;
    cfg.issuer = "bronx"; cfg.audience = "gw";
    JwtAuthenticator auth(cfg);

    std::shared_ptr<GwRequest> holder;
    ReqCtx ctx = makeCtxWithBearer(token, holder);
    CHECK(auth.authenticate(ctx) == AuthErr::Ok);

    std::string sub; ctx.getAttr("jwt_sub", sub);
    CHECK(sub == "bob");
    std::vector<std::string> scopes; ctx.getAttr("jwt_scopes", scopes);
    CHECK(scopes.size() == 2);
    std::cout << "PASS: jwtRs256VerifyAndClaims\n";
}

static void testJwtEs256() {
    std::string token = jwt::create()
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::es256{kTestEcPub, kTestEcPriv, "", ""});
    JwtAuthConfig cfg; cfg.enabled = true; cfg.algo = "ES256"; cfg.pubKey = kTestEcPub;
    JwtAuthenticator auth(cfg);
    std::shared_ptr<GwRequest> holder;
    ReqCtx ctx = makeCtxWithBearer(token, holder);
    CHECK(auth.authenticate(ctx) == AuthErr::Ok);
    std::cout << "PASS: jwtEs256\n";
}

// 过期 token → Expired
static void testJwtExpired() {
    std::string token = jwt::create()
        .set_payload_claim("sub", jwt::claim(std::string("x")))
        .set_expires_at(std::chrono::system_clock::now() - std::chrono::seconds{10})
        .sign(jwt::algorithm::hs256{"s3cret"});
    JwtAuthConfig cfg; cfg.enabled = true; cfg.secret = "s3cret";
    JwtAuthenticator auth(cfg);
    std::shared_ptr<GwRequest> holder;
    ReqCtx ctx = makeCtxWithBearer(token, holder);
    CHECK(auth.authenticate(ctx) == AuthErr::Expired);
    std::cout << "PASS: jwtExpired\n";
}

static void testJwtNeedsExp() {
    JwtAuthConfig cfg; cfg.enabled = true; cfg.secret = "s3cret";
    JwtAuthenticator auth(cfg);
    std::shared_ptr<GwRequest> holder;

    std::string missing = jwt::create()
        .set_payload_claim("sub", jwt::claim(std::string("x")))
        .sign(jwt::algorithm::hs256{"s3cret"});
    ReqCtx missingCtx = makeCtxWithBearer(missing, holder);
    CHECK(auth.authenticate(missingCtx) != AuthErr::Ok);

    std::string bad = jwt::create()
        .set_payload_claim("exp", jwt::claim(std::string("later")))
        .sign(jwt::algorithm::hs256{"s3cret"});
    ReqCtx badCtx = makeCtxWithBearer(bad, holder);
    CHECK(auth.authenticate(badCtx) != AuthErr::Ok);
    std::cout << "PASS: jwtNeedsExp\n";
}

// aud 不符 → BadAud
static void testJwtBadAudience() {
    std::string token = jwt::create()
        .set_audience("other")
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::hs256{"s3cret"});
    JwtAuthConfig cfg; cfg.enabled = true; cfg.secret = "s3cret"; cfg.audience = "gw";
    JwtAuthenticator auth(cfg);
    std::shared_ptr<GwRequest> holder;
    ReqCtx ctx = makeCtxWithBearer(token, holder);
    CHECK(auth.authenticate(ctx) == AuthErr::BadAud);
    std::cout << "PASS: jwtBadAudience\n";
}

// 配 RS256 但 token 是 HS256 → 算法白名单挡掉 BadAlg
static void testJwtAlgMismatch() {
    std::string token = jwt::create()
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::hs256{"s3cret"});
    JwtAuthConfig cfg; cfg.enabled = true; cfg.algo = "RS256"; cfg.pubKey = kTestRsaPub;
    JwtAuthenticator auth(cfg);
    std::shared_ptr<GwRequest> holder;
    ReqCtx ctx = makeCtxWithBearer(token, holder);
    CHECK(auth.authenticate(ctx) == AuthErr::BadAlg);
    std::cout << "PASS: jwtAlgMismatch\n";
}

static void testJwtNone() {
    std::string token = jwt::create()
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::none{});
    JwtAuthConfig cfg; cfg.enabled = true; cfg.secret = "s3cret";
    JwtAuthenticator auth(cfg);
    std::shared_ptr<GwRequest> holder;
    ReqCtx ctx = makeCtxWithBearer(token, holder);
    CHECK(auth.authenticate(ctx) == AuthErr::BadAlg);
    std::cout << "PASS: jwtNone\n";
}

static std::string rsaKidToken(const std::string& kid) {
    auto token = jwt::create()
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600});
    if(!kid.empty()) token.set_key_id(kid);
    return token.sign(jwt::algorithm::rs256{kTestRsaPub, kTestRsaPriv, "", ""});
}

static void testJwtKidKeys() {
    JwtAuthConfig cfg; cfg.enabled = true; cfg.algo = "RS256";
    cfg.keys = {{"old", "RS256", kTestRsaPub}, {"new", "RS256", kTestRsaPub}};
    JwtAuthenticator auth(cfg);
    std::shared_ptr<GwRequest> holder;

    ReqCtx oldCtx = makeCtxWithBearer(rsaKidToken("old"), holder);
    CHECK(auth.authenticate(oldCtx) == AuthErr::Ok);
    ReqCtx newCtx = makeCtxWithBearer(rsaKidToken("new"), holder);
    CHECK(auth.authenticate(newCtx) == AuthErr::Ok);
    ReqCtx noKid = makeCtxWithBearer(rsaKidToken(""), holder);
    CHECK(auth.authenticate(noKid) != AuthErr::Ok);
    ReqCtx unknown = makeCtxWithBearer(rsaKidToken("other"), holder);
    CHECK(auth.authenticate(unknown) != AuthErr::Ok);

    cfg.keys.erase(cfg.keys.begin());
    JwtAuthenticator after(cfg);
    ReqCtx removed = makeCtxWithBearer(rsaKidToken("old"), holder);
    CHECK(after.authenticate(removed) != AuthErr::Ok);

    cfg.pubKey = kTestRsaPub;
    JwtAuthenticator mixed(cfg);
    ReqCtx mixedCtx = makeCtxWithBearer(rsaKidToken("new"), holder);
    CHECK(mixed.authenticate(mixedCtx) != AuthErr::Ok);
    std::cout << "PASS: jwtKidKeys\n";
}

// 造一个带 scope 和 roles 的 HS256 token
static std::string makeJwtWithClaims(const std::string& secret,
                                     const std::string& scope,
                                     const std::vector<std::string>& roles,
                                     const std::string& sub = "bob") {
    picojson::array arr;
    for(const auto& r : roles) arr.push_back(picojson::value(r));
    return jwt::create()
        .set_payload_claim("sub", jwt::claim(sub))
        .set_payload_claim("scope", jwt::claim(scope))
        .set_payload_claim("roles", jwt::claim(picojson::value(arr)))
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::hs256{secret});
}

// 跑一遍 RouteAuth 中间件, 返回是否放行(next 被调), 顺带回填短路状态码
static bool runRouteAuth(const std::string& token, RouteRule rule,
                         const RouteAuthConfig& cfg,
                         std::shared_ptr<GwRequest>& holder, int& status,
                         const std::string& apiKey = "") {
    JwtAuthConfig jcfg; jcfg.enabled = true; jcfg.secret = "s3cret";
    auto jwtAuth = std::make_shared<JwtAuthenticator>(jcfg);
    auto mw = MakeRouteAuthMiddleware(jwtAuth, cfg);

    holder = std::make_shared<GwRequest>();
    holder->setMethod(HttpMethod::GET);
    holder->setMethodRaw("GET");
    holder->setPath("/secure");
    holder->setHeader("Authorization", "Bearer " + token);
    if(!apiKey.empty()) holder->setHeader(cfg.apiKeyHeader, apiKey);
    ReqCtx ctx(holder);
    ctx.route().matched = true;
    ctx.route().rule = std::make_shared<RouteRule>(std::move(rule));

    bool passed = false;
    NextFn next = [&]() { passed = true; };
    mw->handle(ctx, next);
    status = ctx.response() ? ctx.response()->getStatus() : 0;
    // 注入的头留在 holder 上给透传测试查
    holder = ctx.request();
    return passed;
}

// 授权: scope=ALL, roles=ANY, 之间 AND
static void testAuthzScopeRole() {
    RouteAuthConfig cfg; cfg.globalJwtEnabled = true;

    RouteRule rule; rule.authPolicy = "jwt";
    rule.reqScopes = {"read", "write"};
    rule.reqRoles  = {"admin", "ops"};

    std::shared_ptr<GwRequest> h; int st = 0;
    // 全满足 → 放行
    CHECK(runRouteAuth(makeJwtWithClaims("s3cret", "read write x", {"ops"}), rule, cfg, h, st) == true);
    // scope 缺一个 → 403
    CHECK(runRouteAuth(makeJwtWithClaims("s3cret", "read", {"ops"}), rule, cfg, h, st) == false);
    CHECK(st == 403);
    // role 一个都不沾 → 403
    CHECK(runRouteAuth(makeJwtWithClaims("s3cret", "read write", {"guest"}), rule, cfg, h, st) == false);
    CHECK(st == 403);
    std::cout << "PASS: authzScopeRole\n";
}

static void testAuthzNeedsJwt() {
    RouteRule rule;
    rule.reqRoles = {"admin"};
    std::shared_ptr<GwRequest> h;
    int st = 0;

    RouteAuthConfig none;
    rule.authPolicy = "none";
    CHECK(!runRouteAuth("", rule, none, h, st));
    CHECK(st == 403);

    RouteAuthConfig apikey;
    apikey.apiKeys = {"test-key"};
    rule.authPolicy = "api_key";
    CHECK(!runRouteAuth("", rule, apikey, h, st, "test-key"));
    CHECK(st == 403);
    std::cout << "PASS: authzNeedsJwt\n";
}

static std::string sha256Hex(const std::string& s) {
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)s.data(), s.size(), out);
    static const char hex[] = "0123456789abcdef";
    std::string val;
    val.reserve(SHA256_DIGEST_LENGTH * 2);
    for(unsigned char c : out) {
        val.push_back(hex[c >> 4]);
        val.push_back(hex[c & 15]);
    }
    return val;
}

static void testApiKeyHashAndScopes() {
    RouteAuthConfig cfg;
    ApiKey key;
    key.id = "build";
    key.hash = sha256Hex("secret");
    key.scopes = {"read", "write"};
    cfg.keys.push_back(key);
    ApiKey off = key;
    off.id = "off";
    off.enabled = false;
    cfg.keys.push_back(off);

    auto run = [&](const std::string& got, const std::vector<std::string>& needs,
                   const std::vector<std::string>& roles) {
        auto req = std::make_shared<GwRequest>();
        req->setHeader("X-API-Key", got);
        auto ctx = std::make_shared<ReqCtx>(req);
        RouteRule rule; rule.authPolicy = "api_key";
        rule.reqScopes = needs;
        rule.reqRoles = roles;
        ctx->route().rule = std::make_shared<RouteRule>(rule);
        bool passed = false;
        MakeRouteAuthMiddleware(nullptr, cfg)->handle(*ctx, [&]() { passed = true; });
        return std::make_pair(passed, ctx);
    };

    auto good = run("build.secret", {"read"}, {});
    CHECK(good.first);
    std::string id;
    std::vector<std::string> scopes;
    CHECK(good.second->getAttr("api_key_id", id) && id == "build");
    CHECK(good.second->getAttr("auth_scopes", scopes) && scopes.size() == 2);
    auto wrong = run("build.wrong", {}, {});
    CHECK(!wrong.first && wrong.second->response()->getStatus() == 401);
    auto disabled = run("off.secret", {}, {});
    CHECK(!disabled.first && disabled.second->response()->getStatus() == 401);
    auto noScope = run("build.secret", {"admin"}, {});
    CHECK(!noScope.first && noScope.second->response()->getStatus() == 403);
    auto roles = run("build.secret", {}, {"ops"});
    CHECK(!roles.first && roles.second->response()->getStatus() == 403);
    std::cout << "PASS: apiKeyHashAndScopes\n";
}

static YAML::Node authConfigRoot() {
    YAML::Node root;
    root["upstreams"][0]["name"] = "auth-up";
    root["upstreams"][0]["endpoints"][0]["host"] = "127.0.0.1";
    root["upstreams"][0]["endpoints"][0]["port"] = 9;
    root["routes"][0]["name"] = "auth-route";
    root["routes"][0]["path"] = "/auth";
    root["routes"][0]["upstream"] = "auth-up";
    return root;
}

static void testAuthConfigWiring() {
    YAML::Node api = authConfigRoot();
    api["routes"][0]["auth"] = "api_key";
    api["routes"][0]["require_scopes"].push_back("read");
    api["auth"]["api_key"]["keys"][0]["id"] = "build";
    api["auth"]["api_key"]["keys"][0]["hash"] = sha256Hex("secret");
    api["auth"]["api_key"]["keys"][0]["enabled"] = true;
    api["auth"]["api_key"]["keys"][0]["scopes"].push_back("read");
    auto apiSnap = GatewayConfig::BuildSnapshotFromYaml(api);
    auto apiReq = std::make_shared<GwRequest>();
    apiReq->setPath("/auth");
    apiReq->setHeader("X-API-Key", "build.secret");
    apiReq->setWebsocket(true);
    ReqCtx apiCtx(apiReq);
    apiSnap->chain->run(apiCtx);
    std::string id, result;
    CHECK(apiCtx.getAttr("api_key_id", id) && id == "build");
    CHECK(apiCtx.getAttr("auth_result", result) && result == "ok");
    CHECK(!apiReq->hasHeader("X-API-Key"));

    YAML::Node old = authConfigRoot();
    old["routes"][0]["auth"] = "api_key";
    old["auth"]["api_key"]["keys"].push_back("old-key");
    old["auth"]["forward"]["strip_api_key"] = false;
    auto oldSnap = GatewayConfig::BuildSnapshotFromYaml(old);
    auto oldReq = std::make_shared<GwRequest>();
    oldReq->setPath("/auth");
    oldReq->setHeader("X-API-Key", "old-key");
    oldReq->setWebsocket(true);
    ReqCtx oldCtx(oldReq);
    oldSnap->chain->run(oldCtx);
    CHECK(oldCtx.getAttr("auth_result", result) && result == "ok");
    CHECK(oldReq->getHeader("X-API-Key") == "old-key");

    YAML::Node keys = authConfigRoot();
    keys["routes"][0]["auth"] = "jwt";
    keys["auth"]["jwt"]["enabled"] = true;
    keys["auth"]["jwt"]["algo"] = "RS256";
    keys["auth"]["jwt"]["keys"][0]["kid"] = "new";
    keys["auth"]["jwt"]["keys"][0]["algo"] = "RS256";
    keys["auth"]["jwt"]["keys"][0]["public_key"] = kTestRsaPub;
    auto keySnap = GatewayConfig::BuildSnapshotFromYaml(keys);
    auto keyReq = std::make_shared<GwRequest>();
    keyReq->setPath("/auth");
    keyReq->setHeader("Authorization", "Bearer " + rsaKidToken("new"));
    keyReq->setWebsocket(true);
    ReqCtx keyCtx(keyReq);
    keySnap->chain->run(keyCtx);
    CHECK(keyCtx.getAttr("auth_result", result) && result == "ok");
    CHECK(!keyReq->hasHeader("Authorization"));
    std::cout << "PASS: authConfigWiring\n";
}

// 透传: 注入 X-User-*, 注入前剥掉客户端伪造的同名头
static void testFwdUser() {
    RouteAuthConfig cfg; cfg.globalJwtEnabled = true; cfg.fwdUser = true;

    RouteRule rule; rule.authPolicy = "jwt";
    std::shared_ptr<GwRequest> h; int st = 0;
    std::string token = makeJwtWithClaims("s3cret", "read write", {"ops"});
    (void)h; (void)st;

    // 客户端伪造 X-User-Id, 应被剥掉后换成验出来的 bob
    JwtAuthConfig jcfg; jcfg.enabled = true; jcfg.secret = "s3cret";
    auto jwtAuth = std::make_shared<JwtAuthenticator>(jcfg);
    auto mw = MakeRouteAuthMiddleware(jwtAuth, cfg);

    auto req = std::make_shared<GwRequest>();
    req->setMethod(HttpMethod::GET); req->setMethodRaw("GET"); req->setPath("/secure");
    req->setHeader("Authorization", "Bearer " + token);
    req->setHeader("X-User-Id", "evil");   // 伪造
    ReqCtx ctx(req);
    ctx.route().matched = true;
    ctx.route().rule = std::make_shared<RouteRule>(rule);

    bool passed = false;
    NextFn next = [&](){ passed = true; };
    mw->handle(ctx, next);

    CHECK(passed == true);
    CHECK(req->getHeader("X-User-Id") == "bob");           // 伪造被换掉
    CHECK(req->getHeader("X-User-Scopes") == "read,write"); // 注入 scope
    CHECK(req->getHeader("X-User-Roles") == "ops");
    CHECK(!req->hasHeader("Authorization"));
    std::cout << "PASS: fwdUser\n";
}

// 公开路由(policy=none)也得剥掉客户端伪造的 X-User-*, 不然直穿上游
static void testFwdUserStripsOnPublicRoute() {
    RouteAuthConfig cfg; cfg.globalJwtEnabled = false; cfg.fwdUser = true;
    auto mw = MakeRouteAuthMiddleware(nullptr, cfg);

    auto req = std::make_shared<GwRequest>();
    req->setMethod(HttpMethod::GET); req->setMethodRaw("GET"); req->setPath("/open");
    req->setHeader("X-User-Id", "evil");
    req->setHeader("X-User-Roles", "admin");
    req->setHeader("Authorization", "Bearer client-value");
    ReqCtx ctx(req);
    ctx.route().matched = true;
    ctx.route().rule = std::make_shared<RouteRule>();   // authPolicy 默认 inherit → none

    bool passed = false;
    NextFn next = [&](){ passed = true; };
    mw->handle(ctx, next);

    CHECK(passed == true);                          // 公开路由放行
    CHECK(!req->hasHeader("X-User-Id"));            // 伪造被剥
    CHECK(!req->hasHeader("X-User-Roles"));
    CHECK(req->getHeader("Authorization") == "Bearer client-value");
    std::cout << "PASS: fwdUserStripsOnPublicRoute\n";
}

static void testApiKeyKeepsAuthorization() {
    RouteAuthConfig cfg; cfg.apiKeys = {"good"};
    auto req = std::make_shared<GwRequest>();
    req->setHeader("X-API-Key", "good");
    req->setHeader("Authorization", "Bearer client-value");
    ReqCtx ctx(req);
    RouteRule rule; rule.authPolicy = "api_key";
    ctx.route().rule = std::make_shared<RouteRule>(rule);
    bool passed = false;
    MakeRouteAuthMiddleware(nullptr, cfg)->handle(ctx, [&]() { passed = true; });
    CHECK(passed);
    CHECK(req->getHeader("Authorization") == "Bearer client-value");
    std::cout << "PASS: apiKeyKeepsAuthorization\n";
}

static void testApiKeyStripsHeader() {
    RouteAuthConfig cfg; cfg.apiKeys = {"good"};
    auto req = std::make_shared<GwRequest>();
    req->addHeader("X-API-Key", "bad");
    req->addHeader("x-api-key", "good");
    req->setHeader("Authorization", "Bearer client-value");
    ReqCtx ctx(req);
    RouteRule rule; rule.authPolicy = "api_key";
    ctx.route().rule = std::make_shared<RouteRule>(rule);
    bool passed = false;
    MakeRouteAuthMiddleware(nullptr, cfg)->handle(ctx, [&]() { passed = true; });
    CHECK(passed);
    CHECK(!req->hasHeader("X-API-Key"));
    CHECK(req->dumpHead().find("API-Key") == std::string::npos);
    CHECK(req->getHeader("Authorization") == "Bearer client-value");

    cfg.stripApiKey = false;
    req = std::make_shared<GwRequest>();
    req->setHeader("X-API-Key", "good");
    ReqCtx kept(req);
    kept.route().rule = std::make_shared<RouteRule>(rule);
    passed = false;
    MakeRouteAuthMiddleware(nullptr, cfg)->handle(kept, [&]() { passed = true; });
    CHECK(passed && req->getHeader("X-API-Key") == "good");

    RouteAuthConfig raw; raw.apiKeys = {"good"};
    req = std::make_shared<GwRequest>();
    req->setHeader("X-API-Key", "good" + std::string(256, '\0'));
    ReqCtx bad(req);
    bad.route().rule = std::make_shared<RouteRule>(rule);
    passed = false;
    MakeRouteAuthMiddleware(nullptr, raw)->handle(bad, [&]() { passed = true; });
    CHECK(!passed && bad.response()->getStatus() == 401);
    std::cout << "PASS: apiKeyStripsHeader\n";
}

static void testJwtBadAlgoFailsClosed() {
    YAML::Node root = authConfigRoot();
    root["routes"][0]["auth"] = "jwt";
    root["auth"]["jwt"]["enabled"] = true;
    root["auth"]["jwt"]["algo"] = "not-jwt";
    root["auth"]["jwt"]["secret"] = "s3cret";
    auto snap = GatewayConfig::BuildSnapshotFromYaml(root);
    auto req = std::make_shared<GwRequest>();
    req->setPath("/auth");
    req->setHeader("Authorization", "Bearer " + makeJwtWithClaims("s3cret", "", {}));
    req->setWebsocket(true);
    ReqCtx ctx(req);
    snap->chain->run(ctx);
    std::string result;
    CHECK(ctx.response() && ctx.response()->getStatus() == 401);
    CHECK(ctx.getAttr("auth_result", result) && result == "bad_alg");

    root["auth"]["jwt"]["algo"] = " hs256 ";
    snap = GatewayConfig::BuildSnapshotFromYaml(root);
    req = std::make_shared<GwRequest>();
    req->setPath("/auth");
    req->setHeader("Authorization", "Bearer " + makeJwtWithClaims("s3cret", "", {}));
    req->setWebsocket(true);
    ReqCtx ok(req);
    snap->chain->run(ok);
    CHECK(ok.getAttr("auth_result", result) && result == "ok");
    std::cout << "PASS: jwtBadAlgoFailsClosed\n";
}

static void testFwdUserDropsOddClaims() {
    RouteAuthConfig cfg; cfg.globalJwtEnabled = true; cfg.fwdUser = true;
    RouteRule rule; rule.authPolicy = "jwt";
    std::shared_ptr<GwRequest> h;
    int st = 0;
    std::string token = makeJwtWithClaims("s3cret", "read",
        {"ops", "guest,admin"}, "bob\r\nX-Poison: yes");

    CHECK(!runRouteAuth(token, rule, cfg, h, st));
    CHECK(st == 401);
    CHECK(h->dumpHead().find("\r\nX-Poison: yes\r\n") == std::string::npos);
    std::cout << "PASS: fwdUserDropsOddClaims\n";
}

// sub 不是串(数字)也不能崩, 当空处理, token 本身还是验过放行
static void testJwtNonStringSub() {
    std::string token = jwt::create()
        .set_payload_claim("sub", jwt::claim(picojson::value(12345.0)))
        .set_payload_claim("scope", jwt::claim(std::string("read")))
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::hs256{"s3cret"});
    JwtAuthConfig cfg; cfg.enabled = true; cfg.secret = "s3cret";
    JwtAuthenticator auth(cfg);
    std::shared_ptr<GwRequest> holder;
    ReqCtx ctx = makeCtxWithBearer(token, holder);
    CHECK(auth.authenticate(ctx) == AuthErr::Ok);   // 不再误判 NoToken
    std::string sub; ctx.getAttr("jwt_sub", sub);
    CHECK(sub.empty());                              // 抽不出就留空
    std::vector<std::string> scopes; ctx.getAttr("jwt_scopes", scopes);
    CHECK(scopes.size() == 1);                       // 其余声明照抽
    std::cout << "PASS: jwtNonStringSub\n";
}

// Bearer scheme 大小写不敏感, 小写 bearer 也认
static void testJwtBearerCaseInsensitive() {
    std::string token = jwt::create()
        .set_payload_claim("sub", jwt::claim(std::string("y")))
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{3600})
        .sign(jwt::algorithm::hs256{"s3cret"});
    JwtAuthConfig cfg; cfg.enabled = true; cfg.secret = "s3cret";
    JwtAuthenticator auth(cfg);
    auto req = std::make_shared<GwRequest>();
    req->setMethod(HttpMethod::GET); req->setMethodRaw("GET"); req->setPath("/secure");
    req->setHeader("Authorization", "bearer " + token);   // 小写
    ReqCtx ctx(req);
    CHECK(auth.authenticate(ctx) == AuthErr::Ok);
    std::cout << "PASS: jwtBearerCaseInsensitive\n";
}

static void testHealthSkipsUnhealthyEndpoint() {
    CircuitBreakerConfig cb;
    ConnPoolConfig pool;
    auto ep0 = std::make_shared<Endpoint>("127.0.0.1", 9, 1, cb, pool);
    auto ep1 = std::make_shared<Endpoint>("127.0.0.1", 9, 1, cb, pool);
    ep0->healthy.store(false);
    ep1->healthy.store(true);
    auto g = std::make_shared<UpstreamGroup>("h", MakeLoadBalancer("round_robin"));
    g->addEndpoint(ep0);
    g->addEndpoint(ep1);
    auto eps = g->endpoints();
    CHECK(eps.size() == 2);
    CHECK(!eps[0]->healthy.load());
    CHECK(eps[1]->healthy.load());
    std::cout << "PASS: healthStateExport\n";
}

static void testTryAcquireDoesNotSpendUnselectedHalfOpenProbe() {
    OneShotListener listener;
    CircuitBreakerConfig cb;
    cb.failureThreshold = 1;
    cb.openTimeoutMs = 1;
    cb.halfOpenMaxRequests = 1;
    ConnPoolConfig pool;

    auto halfOpenCandidate = std::make_shared<Endpoint>("127.0.0.1", 9, 1, cb, pool);
    auto selectedCandidate = std::make_shared<Endpoint>("127.0.0.1", listener.port, 100, cb, pool);
    halfOpenCandidate->cb->recordFailure();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto g = std::make_shared<UpstreamGroup>("cb-lb", MakeLoadBalancer("weighted"));
    g->addEndpoint(halfOpenCandidate);
    g->addEndpoint(selectedCandidate);

    auto conn = g->tryAcquire(500);
    CHECK(conn.endpoint == selectedCandidate.get());
    g->release(conn, true, false);

    CHECK(halfOpenCandidate->cb->isAllowed());
    std::cout << "PASS: upstreamAcquireKeepsUnselectedHalfOpenProbe\n";
}

static bronx::BxAddress::ptr loopbackAddr(uint16_t port) {
    auto addr = bronx::BxAddress::ResolveOneIp("127.0.0.1");
    CHECK(addr != nullptr);
    addr->setPort(port);
    return addr;
}

static void testConnectionPoolReuseAndReleaseValidation() {
    {
        OneShotListener listener(1, false);
        ConnPoolConfig cfg;
        cfg.maxIdle = 1;
        cfg.idleTimeoutMs = 1000;
        ConnectionPool pool(loopbackAddr(listener.port), cfg);
        bool fromPool = true;
        auto sock = pool.acquire(500, &fromPool);
        CHECK(sock != nullptr);
        CHECK(!fromPool);
        pool.release(sock, true);
        CHECK(pool.idleCount() == 1);
        fromPool = false;
        auto reused = pool.acquire(500, &fromPool);
        CHECK(reused != nullptr);
        CHECK(fromPool);
        pool.release(reused, false);
    }
    {
        OneShotListener listener(1, true);
        ConnPoolConfig cfg;
        cfg.maxIdle = 1;
        cfg.idleTimeoutMs = 1000;
        ConnectionPool pool(loopbackAddr(listener.port), cfg);
        bool fromPool = true;
        auto sock = pool.acquire(500, &fromPool);
        CHECK(sock != nullptr);
        CHECK(!fromPool);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        pool.release(sock, true);
        CHECK(pool.idleCount() == 0);
    }
    {
        OneShotListener listener(1, false);
        ConnPoolConfig cfg;
        cfg.maxIdle = 0;
        cfg.idleTimeoutMs = 1000;
        ConnectionPool pool(loopbackAddr(listener.port), cfg);
        bool fromPool = true;
        auto sock = pool.acquire(500, &fromPool);
        CHECK(sock != nullptr);
        CHECK(!fromPool);
        pool.release(sock, true);
        CHECK(pool.idleCount() == 0);
    }
    std::cout << "PASS: connectionPoolReuseAndReleaseValidation\n";
}

static void testGatewayConfigLoadBalancerValidation() {
    OneShotListener invalidLbFirst;
    OneShotListener invalidLbSecond;
    OneShotListener runtimeCfgListener;

    std::string yaml = R"(
upstreams:
  - name: invalid_lb
    lb: does_not_exist
    endpoints:
      - host: 127.0.0.1
        port: )" + std::to_string(invalidLbFirst.port) + R"(
        weight: 1
      - host: 127.0.0.1
        port: )" + std::to_string(invalidLbSecond.port) + R"(
        weight: 1
  - name: bad_weights
    lb: weighted
    endpoints:
      - host: 127.0.0.1
        port: 9
        weight: 0
      - host: 127.0.0.1
        port: 9
        weight: -4
      - host: 127.0.0.1
        port: 9
        weight: 2000000
  - name: bad_runtime
    lb: round_robin
    circuit_breaker:
      failure_threshold: 0
      open_timeout_ms: 0
      half_open_max_requests: 0
    connection_pool:
      max_idle: -3
      idle_timeout_ms: 0
    health_check:
      enabled: true
      path: /healthz
      interval_ms: 0
      timeout_ms: -1
      healthy_threshold: 0
      unhealthy_threshold: -2
    endpoints:
      - host: 127.0.0.1
        port: )" + std::to_string(runtimeCfgListener.port) + R"(
        weight: 1
routes: []
)";
    YAML::Node root = YAML::Load(yaml);
    auto snap = GatewayConfig::BuildSnapshotFromYaml(root);
    auto invalid = snap->upstreams->get("invalid_lb");
    CHECK(invalid != nullptr);
    CHECK(invalid->endpointCount() == 2);
    auto first = invalid->tryAcquire(500);
    CHECK(first.endpoint != nullptr);
    uint32_t firstPort = first.endpoint->port;
    invalid->release(first, true, false);
    auto second = invalid->tryAcquire(500);
    CHECK(second.endpoint != nullptr);
    uint32_t secondPort = second.endpoint->port;
    invalid->release(second, true, false);
    CHECK(firstPort == invalidLbFirst.port);
    CHECK(secondPort == invalidLbSecond.port);

    auto badWeights = snap->upstreams->get("bad_weights");
    CHECK(badWeights != nullptr);
    auto eps = badWeights->endpoints();
    CHECK(eps.size() == 3);
    CHECK(eps[0]->weight == 1);
    CHECK(eps[1]->weight == 1);
    CHECK(eps[2]->weight == kMaxLoadBalancerWeight);

    auto runtime = snap->upstreams->get("bad_runtime");
    CHECK(runtime != nullptr);
    CHECK(runtime->endpointCount() == 1);
    CHECK(runtime->healthCheck().enabled);
    CHECK(runtime->healthCheck().intervalMs == 1);
    CHECK(runtime->healthCheck().timeoutMs == 1);
    CHECK(runtime->healthCheck().healthyThreshold == 1);
    CHECK(runtime->healthCheck().unhealthyThreshold == 1);
    auto runtimeConn = runtime->tryAcquire(500);
    CHECK(runtimeConn.endpoint != nullptr);
    auto runtimeEp = runtimeConn.endpoint;
    runtime->release(runtimeConn, true, true);
    CHECK(runtimeEp->pool->idleCount() == 0);
    runtimeEp->cb->recordFailure();
    CHECK(runtimeEp->cb->state() == CircuitBreaker::State::OPEN);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(runtimeEp->cb->isAllowed());
    CHECK(!runtimeEp->cb->isAllowed());
    std::cout << "PASS: gatewayConfigLoadBalancerValidation\n";
}

static void testUpstreamConfigLimits() {
    std::string yaml = R"(
upstreams:
  - name: governed
    lb: weighted_least_conn
    timeout:
      total_ms: 0
      connect_ms: -2
      read_ms: 0
    limits:
      max_inflight: -5
    circuit_breaker:
      failure_threshold: 9
      window_ms: 0
      buckets: 9000
      min_requests: 0
      failure_rate: 120
      slow_ms: 0
      slow_rate: -4
      open_timeout_ms: 10
      max_open_timeout_ms: 5
      half_open_max_requests: 3
      half_open_successes: 8
      failure_statuses: [500, 99, 429, 700]
    endpoints:
      - host: 127.0.0.1
        port: 9
        weight: 2
routes: []
)";
    auto snap = GatewayConfig::BuildSnapshotFromYaml(YAML::Load(yaml));
    auto group = snap->upstreams->get("governed");
    CHECK(group != nullptr);
    CHECK(std::string(group->lbName()) == "weighted_least_conn");
    CHECK(group->totalMs() == 1);
    CHECK(group->connectMs() == 1);
    CHECK(group->readMs() == 1);
    auto eps = group->endpoints();
    CHECK(eps.size() == 1);
    CHECK(eps[0]->maxInflight == 0);
    const auto& cfg = eps[0]->cb->config();
    CHECK(cfg.windowMs == 1);
    CHECK(cfg.buckets == 1024);
    CHECK(cfg.minRequests == 1);
    CHECK(cfg.failureRate == 100);
    CHECK(cfg.slowMs == 1);
    CHECK(cfg.slowRate == 0);
    CHECK(cfg.maxOpenTimeoutMs == 10);
    CHECK(cfg.halfOpenSuccesses == 3);
    CHECK(cfg.failureStatuses.size() == 2);
    CHECK(eps[0]->cb->badStatus(500));
    CHECK(eps[0]->cb->badStatus(429));
    CHECK(!eps[0]->cb->badStatus(503));
    std::cout << "PASS: upstreamConfigLimits\n";
}

static void testGatewayConfigRouteValidation() {
    OneShotListener listener;

    std::string yaml = R"(
upstreams:
  - name: backend
    lb: round_robin
    endpoints:
      - host: 127.0.0.1
        port: )" + std::to_string(listener.port) + R"(
routes:
  - name: bad-match
    match_type: typo
    path: /bad
    upstream: backend
  - name: normalized
    match_type: " EXACT "
    path: public
    upstream: backend
    rewrite_prefix: v1
    auth:
      policy: " API_KEY "
  - name: bad-auth
    path: /closed
    upstream: backend
    auth: no_such_policy
  - name: bad-needs
    path: /bad-needs
    upstream: backend
    auth: jwt
    require_roles: admin
)";
    YAML::Node root = YAML::Load(yaml);
    auto snap = GatewayConfig::BuildSnapshotFromYaml(root);
    auto routes = snap->router->listRoutes();
    CHECK(routes.size() == 2);

    const RouteRule* normalized = nullptr;
    const RouteRule* badAuth = nullptr;
    for(const auto& r : routes) {
        if(r.name == "normalized") normalized = &r;
        if(r.name == "bad-auth") badAuth = &r;
    }
    CHECK(normalized != nullptr);
    CHECK(normalized->matchType == MatchType::EXACT);
    CHECK(normalized->pathPattern == "/public");
    CHECK(normalized->rewritePrefix == "/v1");
    CHECK(normalized->authPolicy == "api_key");
    CHECK(badAuth != nullptr);
    CHECK(badAuth->authPolicy == "no_such_policy");
    std::cout << "PASS: gatewayConfigRouteValidation\n";
}

static void testGatewayConfigCorsOptions() {
    std::string yaml = R"(
cors:
  enabled: true
  allow_origins:
    - https://allowed.example
  allow_methods: "GET, POST, OPTIONS"
  allow_headers: "Content-Type, X-Token"
  allow_credentials: true
  max_age: 123
upstreams: []
routes: []
)";
    YAML::Node root = YAML::Load(yaml);
    auto snap = GatewayConfig::BuildSnapshotFromYaml(root);
    auto req = std::make_shared<GwRequest>();
    req->setMethod(HttpMethod::OPTIONS);
    req->setPath("/api");
    req->setHeader("Origin", "https://allowed.example");
    req->setHeader("Access-Control-Request-Method", "POST");
    req->setHeader("Access-Control-Request-Headers", "X-Token");
    ReqCtx ctx(req);
    snap->chain->run(ctx);

    CHECK(ctx.isHandled());
    CHECK(ctx.response() != nullptr);
    CHECK(ctx.response()->getStatus() == 204);
    CHECK(ctx.response()->getHeader("Access-Control-Allow-Origin") == "https://allowed.example");
    CHECK(ctx.response()->getHeader("Access-Control-Allow-Credentials") == "true");
    CHECK(ctx.response()->getHeader("Access-Control-Allow-Headers") == "X-Token");
    CHECK(ctx.response()->getHeader("Access-Control-Max-Age") == "123");
    CHECK(ctx.response()->getHeader("Vary") == "Origin");

    std::string hugeAgeYaml = R"(
cors:
  max_age: 999999999999
upstreams: []
routes: []
)";
    auto hugeSnap = GatewayConfig::BuildSnapshotFromYaml(YAML::Load(hugeAgeYaml));
    auto hugeReq = std::make_shared<GwRequest>();
    hugeReq->setMethod(HttpMethod::OPTIONS);
    hugeReq->setPath("/api");
    hugeReq->setHeader("Origin", "https://any.example");
    hugeReq->setHeader("Access-Control-Request-Method", "POST");
    ReqCtx hugeCtx(hugeReq);
    hugeSnap->chain->run(hugeCtx);
    CHECK(hugeCtx.response() != nullptr);
    CHECK(hugeCtx.response()->getHeader("Access-Control-Max-Age")
          == std::to_string(std::numeric_limits<int>::max()));
    std::cout << "PASS: gatewayConfigCorsOptions\n";
}

static std::string writeTempGatewayConfig(const std::string& suffix,
                                          const std::string& routeName,
                                          const std::string& path) {
    std::string file = "/tmp/bronx_gateway_reload_" + suffix + ".yml";
    std::ofstream out(file, std::ios::trunc);
    CHECK(out.good());
    out << "upstreams:\n"
        << "  - name: backend\n"
        << "    lb: round_robin\n"
        << "    endpoints:\n"
        << "      - host: 127.0.0.1\n"
        << "        port: 9\n"
        << "routes:\n"
        << "  - name: " << routeName << "\n"
        << "    path: " << path << "\n"
        << "    upstream: backend\n";
    out.close();
    CHECK(out.good());
    return file;
}

static void testGatewayReloadSwapsSnapshotAndKeepsOldOnFailure() {
    std::string cfg1 = writeTempGatewayConfig("one", "r1", "/v1");
    std::string cfg2 = writeTempGatewayConfig("two", "r2", "/v2");
    std::string missing = "/tmp/bronx_gateway_reload_missing.yml";
    ::unlink(missing.c_str());

    GatewayServer gw;
    gw.setCfgPath(cfg1);
    CHECK(gw.reload(""));
    CHECK(gw.cfgPath() == cfg1);
    auto snap1 = gw.getConfig();
    CHECK(snap1 && snap1->router);
    auto routes1 = snap1->router->listRoutes();
    CHECK(routes1.size() == 1);
    CHECK(routes1[0].name == "r1");
    CHECK(routes1[0].pathPattern == "/v1");

    CHECK(gw.reload(cfg2));
    CHECK(gw.cfgPath() == cfg2);
    auto snap2 = gw.getConfig();
    CHECK(snap2 && snap2 != snap1);
    auto routes2 = snap2->router->listRoutes();
    CHECK(routes2.size() == 1);
    CHECK(routes2[0].name == "r2");
    CHECK(routes2[0].pathPattern == "/v2");

    CHECK(!gw.reload(missing));
    CHECK(gw.cfgPath() == cfg2);
    auto snapAfterFail = gw.getConfig();
    CHECK(snapAfterFail == snap2);

    std::string cfg3 = writeTempGatewayConfig("two", "r3", "/v3");
    CHECK(cfg3 == cfg2);
    CHECK(gw.reload(""));
    auto snap3 = gw.getConfig();
    CHECK(snap3 && snap3 != snap2);
    auto routes3 = snap3->router->listRoutes();
    CHECK(routes3.size() == 1);
    CHECK(routes3[0].name == "r3");
    CHECK(routes3[0].pathPattern == "/v3");

    ::unlink(cfg1.c_str());
    ::unlink(cfg2.c_str());
    std::cout << "PASS: gatewayReloadSwapsSnapshotAndKeepsOldOnFailure\n";
}

// ─── MemAlloc ─────────────────────────────────────────────────────────────────

static void testMemAlloc() {
    // 默认 new/delete
    void* p = bronx::gateway::MemAlloc::alloc(64);
    CHECK(p != nullptr);
    bronx::gateway::MemAlloc::free(p, 64);
    // 自定义 allocator
    int alloc_count = 0, free_count = 0;
    bronx::gateway::MemAlloc::setAllocator(
        [&](size_t n) -> void* { ++alloc_count; return ::operator new(n); },
        [&](void* q, size_t)   { ++free_count;  ::operator delete(q); });
    void* q = bronx::gateway::MemAlloc::alloc(32);
    bronx::gateway::MemAlloc::free(q, 32);
    CHECK(alloc_count == 1 && free_count == 1);
    // 还原默认
    bronx::gateway::MemAlloc::setAllocator(nullptr, nullptr);
    std::cout << "PASS: memAlloc\n";
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    // Router
    testRouterExact();
    testRouterPrefix();
    testRouterMethodFilter();
    testRouterExtendedMethodRaw();
    testRouterTieBreak();
    testRouterGenerationCacheInvalidation();
    testRouterDefaultArcCache();
    testRouterListRoutes();
    testRouterSetTableThenAddRoutePreservesExistingRules();
    testRouterKeyExtractorAndNullCache();
    testMiddlewareShortCircuitWithoutConnection();
    // CircuitBreaker
    testCircuitBreakerClosed();
    testCircuitBreakerHalfOpen();
    testCircuitBreakerHalfOpenFail();
    testCircuitBreakerHalfOpenMaxRequests();
    testCircuitBreakerOpenIgnoresStaleSuccess();
    testCircuitBreakerConfigClamp();
    testUpRetSkipAndWindow();
    testCircuitSlowAndHalfOpenTurn();
    testEndpointHoldAndUpStat();
    testWeightLeast();
    // Metrics
    testMetricsConcurrent();
    testMetricsLatencyPercentiles();
    // TokenBucket
    testTokenBucket();
    // IPFilter
    testIPFilter();
    testJwtEmptySecretFailsClosed();
    testJwtRs256VerifyAndClaims();
    testJwtEs256();
    testJwtExpired();
    testJwtNeedsExp();
    testJwtBadAudience();
    testJwtAlgMismatch();
    testJwtNone();
    testJwtKidKeys();
    testAuthMetrics();
    testAuthzScopeRole();
    testAuthzNeedsJwt();
    testApiKeyHashAndScopes();
    testAuthConfigWiring();
    testFwdUser();
    testFwdUserStripsOnPublicRoute();
    testApiKeyKeepsAuthorization();
    testApiKeyStripsHeader();
    testJwtBadAlgoFailsClosed();
    testFwdUserDropsOddClaims();
    testJwtNonStringSub();
    testJwtBearerCaseInsensitive();
    testHealthSkipsUnhealthyEndpoint();
    testTryAcquireDoesNotSpendUnselectedHalfOpenProbe();
    testConnectionPoolReuseAndReleaseValidation();
    testGatewayConfigLoadBalancerValidation();
    testUpstreamConfigLimits();
    testGatewayConfigRouteValidation();
    testGatewayConfigCorsOptions();
    testGatewayReloadSwapsSnapshotAndKeepsOldOnFailure();
    // MemAlloc
    testMemAlloc();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
