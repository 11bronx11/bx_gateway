// G4:内置中间件
// ===============
// 短路类(无需上游):CORS 预检 204、重定向 302、健康检查 200、限流 429。
// 头注入类(经代理到上游,查响应头):安全头(CSP 等)、request-id。

#include "test_util.h"
#include "gateway.h"
#include "conn.h"
#include "ctx.h"
#include "mw.h"
#include "middlewares/builtin.h"
#include "middlewares/proxy.h"
#include "router.h"
#include "ups_group.h"
#include "metrics.h"
#include "lb.h"
#include "reactor.h"
#include "endpoint.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <sstream>
#include <fstream>

using namespace bronx::gateway;
static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

static const uint16_t GW_PORT = 18800;
static const uint16_t UP_PORT = 18801;

static void upstream_handler(GatewayConnection& c) {
    auto reader = c.requestBody();
    std::string rb; if(reader){ int n; while((n=reader->readChunk(rb))>0){} }
    auto rsp = std::make_shared<GwResponse>();
    rsp->setStatus(200);
    c.sendResponse(rsp, "upstream-ok");
}

static GatewayServer::RequestHandler chainHandler(MwChain::ptr chain) {
    return [chain](GatewayConnection& c){ ReqCtx ctx(&c); chain->run(ctx); };
}

static int cfd(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port);
    inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    if(connect(fd,(sockaddr*)&a,sizeof(a))!=0){close(fd);return -1;} return fd;
}
static std::string recvall(int fd){ std::string o; char b[2048]; int n;
    while((n=recv(fd,b,sizeof(b),0))>0) o.append(b,n); return o; }

// request-id 存进 ctx,access log 从 ctx 拿来写进 JSON,拿一个 id 就能串网关这环日志。
// 无连接 ReqCtx 跑 request-id + access-log,把 system logger 引到临时文件,读回来查。
static void checkReqIdInAccessLog() {
    std::string logFile = "/tmp/bronx_alog_reqid.log";
    ::unlink(logFile.c_str());
    auto sys = BRONX_LOG_NAME("system");
    auto appender = std::make_shared<bronx::BxFileLogAppender>(logFile);
    sys->addAppender(appender);
    sys->setLevel(bronx::BxLogLevel::INFO);

    auto chain = std::make_shared<MwChain>();
    chain->use(MakeRequestIdMiddleware());
    chain->use(MakeStructuredAccessLogMiddleware());
    chain->use([](ReqCtx& ctx, const NextFn&) {
        auto rsp = std::make_shared<GwResponse>();
        rsp->setStatus(200);
        ctx.setResponse(rsp);
        ctx.setHandled(true);
    }, "sink");

    auto req = std::make_shared<GwRequest>();
    req->setMethod(HttpMethod::GET);
    req->setPath("/trace/me");
    ReqCtx ctx(req);
    chain->run(ctx);

    appender->flush();
    usleep(50*1000);
    std::ifstream in(logFile);
    std::string line, all;
    while(std::getline(in, line)) all += line;

    // req 无该头 → 中间件现生成,日志里 request_id 应非空
    std::string got;
    TEST_CHECK_MSG(ctx.getAttr("request_id", got) && !got.empty(),
                   "request-id middleware stores non-empty id in ctx");
    TEST_CHECK_MSG(all.find("\"request_id\":\"" + got + "\"") != std::string::npos,
                   "access log json carries the same request_id: " << all);

    sys->delAppender(appender);
    ::unlink(logFile.c_str());
}
static std::string ex(uint16_t port, const std::string& w){
    int fd=cfd(port); if(fd<0) return ""; send(fd,w.data(),w.size(),0);
    std::string r=recvall(fd); close(fd); return r; }
static int statusOf(const std::string& resp) {
    auto sp1 = resp.find(' ');
    if(sp1 == std::string::npos) return 0;
    auto sp2 = resp.find(' ', sp1 + 1);
    if(sp2 == std::string::npos) return 0;
    return std::stoi(resp.substr(sp1 + 1, sp2 - sp1 - 1));
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_gw_builtin start ===";
    bronx::BxIoManager iom(3);

    // 上游
    auto up = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    up->setRequestHandler(upstream_handler);
    up->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(UP_PORT)));
    up->start();

    // 网关链:health → cors → redirect → security → request-id → rate-limit → router → proxy
    auto router = std::make_shared<Router>();
    auto upReg = std::make_shared<UpstreamRegistry>();
    {
        auto ep = std::make_shared<Endpoint>("127.0.0.1", UP_PORT, 1, CircuitBreakerConfig{}, ConnPoolConfig{});
        auto grp = std::make_shared<UpstreamGroup>("backend", MakeLoadBalancer("round_robin"));
        grp->addEndpoint(ep);
        upReg->add(grp);
    }
    RouteRule r; r.name="api"; r.pathPattern="/api"; r.upstream="backend"; r.stripPrefix=true;
    router->addRoute(r);
    auto chain = std::make_shared<MwChain>();
    chain->use(MakeHealthCheckMiddleware("/healthz"));
    chain->use(MakeCorsMiddleware());
    std::vector<RedirectRule> reds = {{"/old", "https://new.example/", 301}};
    chain->use(MakeRedirectMiddleware(reds));
    chain->use(MakeSecurityHeadersMiddleware());
    chain->use(MakeRequestIdMiddleware());
    chain->use(MakeRateLimitMiddleware(3, 0.0001));   // 容量3,几乎不补充
    chain->use(MakeRouterMiddleware(router, upReg));
    chain->use(MakeProxyMiddleware(2000, 5000));

    auto gw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    gw->setRequestHandler(chainHandler(chain));
    gw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(GW_PORT)));
    gw->start();
    usleep(200*1000);

    // 1. 健康检查 → 200 OK
    {
        std::string resp = ex(GW_PORT, "GET /healthz HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find("200")!=std::string::npos && resp.find("OK")!=std::string::npos,
                       "health 200: " << resp.substr(0,40));
    }
    // 2. CORS 预检 OPTIONS → 204 + ACAO
    {
        std::string resp = ex(GW_PORT, "OPTIONS /api/x HTTP/1.1\r\n"
                                      "Host: x\r\n"
                                      "Origin: https://app.example\r\n"
                                      "Access-Control-Request-Method: POST\r\n"
                                      "Connection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find("204")!=std::string::npos, "cors 204: " << resp.substr(0,40));
        TEST_CHECK_MSG(resp.find("Access-Control-Allow-Origin: *")!=std::string::npos, "ACAO header");
        TEST_CHECK_MSG(resp.find("Content-Security-Policy") == std::string::npos,
                       "preflight happens before security middleware in this chain");
    }
    // 3. 重定向 → 301 + Location
    {
        std::string resp = ex(GW_PORT, "GET /old/page HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find("301")!=std::string::npos, "redirect 301: " << resp.substr(0,40));
        TEST_CHECK_MSG(resp.find("Location: https://new.example/")!=std::string::npos, "Location header");
    }
    // 4. 代理 + CORS 实际响应头 + 安全头 + request-id(经上游响应,查注入的响应头)
    {
        std::string resp = ex(GW_PORT, "GET /api/data HTTP/1.1\r\n"
                                      "Host: x\r\n"
                                      "Origin: https://app.example\r\n"
                                      "Connection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find("upstream-ok")!=std::string::npos, "proxied body: " << resp.substr(0,60));
        TEST_CHECK_MSG(resp.find("Access-Control-Allow-Origin")!=std::string::npos, "CORS actual response injected");
        TEST_CHECK_MSG(resp.find("Content-Security-Policy")!=std::string::npos, "CSP header injected");
        TEST_CHECK_MSG(resp.find("X-Frame-Options")!=std::string::npos, "X-Frame-Options injected");
        TEST_CHECK_MSG(resp.find("X-Request-Id")!=std::string::npos, "request-id injected");
    }
    // 5. 限流:容量3,连发4次,至少最后一次 429
    {
        int got429 = 0;
        for(int i=0;i<5;++i){
            std::string resp = ex(GW_PORT, "GET /api/rl HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
            if(resp.find("429")!=std::string::npos) got429++;
        }
        TEST_CHECK_MSG(got429 >= 1, "rate limit should 429 after capacity exhausted (got429=" << got429 << ")");
    }
    // 6. 已登记的响应头在后续短路响应中也应合并，不能只在代理回程生效。
    {
        auto scChain = std::make_shared<MwChain>();
        scChain->use(MakeSecurityHeadersMiddleware());
        scChain->use(MakeRequestIdMiddleware());
        scChain->use(MakeRateLimitMiddleware(0, 0));

        uint16_t scPort = GW_PORT + 20;
        auto scGw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        scGw->setRequestHandler(chainHandler(scChain));
        scGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(scPort)));
        scGw->start();
        usleep(150*1000);

        std::string resp = ex(scPort, "GET /limited HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_EQ(statusOf(resp), 429);
        TEST_CHECK_MSG(resp.find("Content-Security-Policy")!=std::string::npos,
                       "short-circuit should include security headers: " << resp);
        TEST_CHECK_MSG(resp.find("X-Request-Id")!=std::string::npos,
                       "short-circuit should include request id: " << resp);
        scGw->stop();
        usleep(150*1000);
    }

    // 7. IPFilter:denylist 按 TCP peer 做安全决策,不能被 X-Forwarded-For 绕过。
    {
        IPFilterConfig cfg;
        cfg.enabled = true;
        cfg.mode = "denylist";
        cfg.cidrs = {"127.0.0.0/8"};
        auto ipChain = std::make_shared<MwChain>();
        ipChain->use(MakeIPFilterMiddleware(cfg));
        ipChain->use([](ReqCtx& ctx, const NextFn&) {
            auto rsp = std::make_shared<GwResponse>();
            rsp->setStatus(200);
            ctx.connection()->sendResponse(rsp, "reached");
            ctx.setHandled(true);
        }, "sink");

        uint16_t ipPort = GW_PORT + 21;
        auto ipGw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        ipGw->setRequestHandler(chainHandler(ipChain));
        ipGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(ipPort)));
        ipGw->start();
        usleep(150*1000);

        std::string resp = ex(ipPort, "GET / HTTP/1.1\r\nHost: x\r\n"
                                      "X-Forwarded-For: 8.8.8.8\r\n"
                                      "Connection: close\r\n\r\n");
        TEST_CHECK_EQ(statusOf(resp), 403);
        TEST_CHECK_MSG(resp.find("reached") == std::string::npos,
                       "denylist should not trust spoofed XFF: " << resp);
        ipGw->stop();
        usleep(150*1000);
    }

    // 8. IPFilter:allowlist 未命中 peer 时拒绝,命中 CIDR 时放行。
    {
        IPFilterConfig cfg;
        cfg.enabled = true;
        cfg.mode = "allowlist";
        cfg.cidrs = {"127.0.0.1/32"};
        auto ipChain = std::make_shared<MwChain>();
        ipChain->use(MakeIPFilterMiddleware(cfg));
        ipChain->use([](ReqCtx& ctx, const NextFn&) {
            auto rsp = std::make_shared<GwResponse>();
            rsp->setStatus(200);
            ctx.connection()->sendResponse(rsp, "allowed");
            ctx.setHandled(true);
        }, "sink");

        uint16_t ipPort = GW_PORT + 22;
        auto ipGw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        ipGw->setRequestHandler(chainHandler(ipChain));
        ipGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(ipPort)));
        ipGw->start();
        usleep(150*1000);

        std::string resp = ex(ipPort, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_EQ(statusOf(resp), 200);
        TEST_CHECK_MSG(resp.find("allowed") != std::string::npos,
                       "allowlist should pass matching peer: " << resp);
        ipGw->stop();
        usleep(150*1000);
    }

    // 9. CORS 白名单 + credentials:回显 Origin,补 Vary,非预检 OPTIONS 不应被短路。
    {
        auto corsChain = std::make_shared<MwChain>();
        CorsOptions cors;
        cors.allowOrigins = {"https://allowed.example"};
        cors.allowCredentials = true;
        cors.allowMethods = "GET, POST, OPTIONS";
        cors.allowHeaders = "Content-Type, X-Token";
        cors.maxAge = 600;
        corsChain->use(MakeCorsMiddleware(cors));
        corsChain->use([](ReqCtx& ctx, const NextFn&) {
            auto rsp = std::make_shared<GwResponse>();
            rsp->setStatus(200);
            for(const auto& h : ctx.extra_resp_hdrs()) {
                rsp->setHeader(h.first, h.second);
            }
            ctx.connection()->sendResponse(rsp, "cors-sink");
            ctx.setHandled(true);
        }, "sink");

        uint16_t corsPort = GW_PORT + 23;
        auto corsGw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        corsGw->setRequestHandler(chainHandler(corsChain));
        corsGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(corsPort)));
        corsGw->start();
        usleep(150*1000);

        // 抓基线,下面三条流量后比 delta(全局单例,别的测试也会加)
        auto m0 = GatewayMetrics::instance().snapshot();

        std::string preflight = ex(corsPort,
            "OPTIONS /resource HTTP/1.1\r\n"
            "Host: x\r\n"
            "Origin: https://allowed.example\r\n"
            "Access-Control-Request-Method: POST\r\n"
            "Access-Control-Request-Headers: X-Token\r\n"
            "Connection: close\r\n\r\n");
        TEST_CHECK_EQ(statusOf(preflight), 204);
        TEST_CHECK_MSG(preflight.find("Access-Control-Allow-Origin: https://allowed.example") != std::string::npos,
                       "credentialed CORS should echo allowed origin: " << preflight);
        TEST_CHECK_MSG(preflight.find("Access-Control-Allow-Credentials: true") != std::string::npos,
                       "credentialed CORS should include ACAC: " << preflight);
        TEST_CHECK_MSG(preflight.find("Vary: Origin") != std::string::npos,
                       "credentialed CORS should vary on Origin: " << preflight);
        TEST_CHECK_MSG(preflight.find("Access-Control-Allow-Headers: X-Token") != std::string::npos,
                       "preflight should echo requested headers: " << preflight);

        std::string denied = ex(corsPort,
            "GET /resource HTTP/1.1\r\n"
            "Host: x\r\n"
            "Origin: https://evil.example\r\n"
            "Connection: close\r\n\r\n");
        TEST_CHECK_EQ(statusOf(denied), 200);
        TEST_CHECK_MSG(denied.find("Access-Control-Allow-Origin") == std::string::npos,
                       "disallowed origin should not get ACAO: " << denied);

        std::string plainOptions = ex(corsPort,
            "OPTIONS /resource HTTP/1.1\r\n"
            "Host: x\r\n"
            "Origin: https://allowed.example\r\n"
            "Connection: close\r\n\r\n");
        TEST_CHECK_EQ(statusOf(plainOptions), 200);
        TEST_CHECK_MSG(plainOptions.find("cors-sink") != std::string::npos,
                       "plain OPTIONS without Access-Control-Request-Method should reach handler: "
                       << plainOptions);

        // 预检短路记 1 次,evil origin 记 1 次 denied
        auto m1 = GatewayMetrics::instance().snapshot();
        TEST_CHECK_EQ(m1.corsPreflight - m0.corsPreflight, 1u);
        TEST_CHECK_EQ(m1.corsDenied - m0.corsDenied, 1u);

        corsGw->stop();
        usleep(150*1000);
    }

    // 10. 维护模式:开着 503 + Retry-After 且不落到 sink,关掉恢复放行。
    {
        auto mChain = std::make_shared<MwChain>();
        mChain->use(MakeMaintenanceMiddleware());
        mChain->use([](ReqCtx& ctx, const NextFn&) {
            auto rsp = std::make_shared<GwResponse>();
            rsp->setStatus(200);
            ctx.connection()->sendResponse(rsp, "maint-sink");
            ctx.setHandled(true);
        }, "sink");

        uint16_t mPort = GW_PORT + 24;
        auto mGw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        mGw->setRequestHandler(chainHandler(mChain));
        mGw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(mPort)));
        mGw->start();
        usleep(150*1000);

        auto m0 = GatewayMetrics::instance().snapshot();

        // 开维护
        MaintGate::instance().set(true);
        std::string blocked = ex(mPort,
            "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_EQ(statusOf(blocked), 503);
        TEST_CHECK_MSG(blocked.find("Retry-After:") != std::string::npos,
                       "maintenance 503 should carry Retry-After: " << blocked);
        TEST_CHECK_MSG(blocked.find("maint-sink") == std::string::npos,
                       "maintenance should short-circuit before sink: " << blocked);

        auto m1 = GatewayMetrics::instance().snapshot();
        TEST_CHECK_EQ(m1.maintBlocked - m0.maintBlocked, 1u);

        // 关维护,恢复放行
        MaintGate::instance().set(false);
        std::string ok = ex(mPort,
            "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_EQ(statusOf(ok), 200);
        TEST_CHECK_MSG(ok.find("maint-sink") != std::string::npos,
                       "after maintenance off request should reach sink: " << ok);

        mGw->stop();
        usleep(150*1000);
    }

    gw->stop(); up->stop();
    usleep(200*1000);

    checkReqIdInAccessLog();

    {
        auto finish = std::make_shared<MwChain>();
        finish->use(MakeRequestIdMiddleware());
        finish->use(MakeSecurityHeadersMiddleware());
        finish->use(MakeFinishMiddleware());
        finish->use([](ReqCtx&, const NextFn&) {}, "empty");
        auto req = std::make_shared<GwRequest>();
        req->setMethod(HttpMethod::GET);
        req->setPath("/empty");
        ReqCtx ctx(req);
        auto before = GatewayMetrics::instance().snapshot();
        finish->run(ctx);
        auto after = GatewayMetrics::instance().snapshot();
        TEST_CHECK_EQ(ctx.status(), 500);
        TEST_CHECK(ctx.respState() == RespState::SENT);
        TEST_CHECK(ctx.response()->hasHeader("X-Request-Id"));
        TEST_CHECK(ctx.response()->hasHeader("Content-Security-Policy"));
        TEST_CHECK_EQ(after.finish500, before.finish500 + 1);
        TEST_CHECK(!reply(ctx, 404));
        TEST_CHECK_EQ(ctx.status(), 500);
    }
    {
        auto req = std::make_shared<GwRequest>();
        ReqCtx ctx(req);
        TEST_CHECK(ctx.commitResp(RespState::STREAM, 200, 1));
        auto before = GatewayMetrics::instance().snapshot();
        writeFail(ctx);
        auto after = GatewayMetrics::instance().snapshot();
        TEST_CHECK(ctx.respState() == RespState::WRITE_FAIL);
        TEST_CHECK_EQ(ctx.status(), 200);
        TEST_CHECK_EQ(ctx.commitMs(), 1u);
        TEST_CHECK_EQ(after.writeFail, before.writeFail + 1);
    }
    return TEST_SUMMARY();
}
