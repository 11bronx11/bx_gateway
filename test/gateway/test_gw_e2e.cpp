// G5 端到端:YAML 配置驱动的完整网关
// ===================================
// 用 YAML 配路由 → GatewayConfig 建 Router → 完整中间件链 → 真实上游往返。
// 这是"配置驱动、不改代码即配路由"的验证,也是完整 HTTP 网关里程碑的端到端证明。

#include "test_util.h"
#include "gateway.h"
#include "conn.h"
#include "conf.h"
#include "admin.h"
#include "ctx.h"
#include "mw.h"
#include "middlewares/builtin.h"
#include "middlewares/proxy.h"
#include "router.h"
#include "ups_group.h"
#include "lb.h"
#include "reactor.h"
#include "endpoint.h"
#include <yaml-cpp/yaml.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>

using namespace bronx::gateway;
static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

static const uint16_t GW_PORT = 18900;
static const uint16_t UP_PORT = 18901;
static const uint16_t ADMIN_PORT = 18902;

static void upstream_handler(GatewayConnection& c) {
    auto reader = c.requestBody();
    std::string rb; if(reader){ int n; while((n=reader->readChunk(rb))>0){} }
    auto rsp = std::make_shared<GwResponse>();
    rsp->setStatus(200);
    c.sendResponse(rsp, "OK:" + c.request()->getPath());
}

static int cfd(uint16_t port){ int fd=socket(AF_INET,SOCK_STREAM,0); sockaddr_in a{};
    a.sin_family=AF_INET; a.sin_port=htons(port); inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    if(connect(fd,(sockaddr*)&a,sizeof(a))!=0){close(fd);return -1;} return fd; }
static std::string ex(uint16_t port, const std::string& w){ int fd=cfd(port); if(fd<0)return "";
    send(fd,w.data(),w.size(),0); std::string o; char b[2048]; int n;
    while((n=recv(fd,b,sizeof(b),0))>0)o.append(b,n); close(fd); return o; }

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_gw_e2e start ===";
    bronx::BxIoManager iom(3);

    auto up = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    up->setRequestHandler(upstream_handler);
    up->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(UP_PORT)));
    up->start();

    // 新格式：用 UpstreamRegistry + Router 配置
    auto upReg0 = std::make_shared<UpstreamRegistry>();
    {
        auto ep  = std::make_shared<Endpoint>("127.0.0.1", UP_PORT, 1, CircuitBreakerConfig{}, ConnPoolConfig{});
        auto grp = std::make_shared<UpstreamGroup>("backend", MakeLoadBalancer("round_robin"));
        grp->addEndpoint(ep);
        upReg0->add(grp);
    }
    auto router = std::make_shared<Router>();
    RouteRule rr; rr.name="svc"; rr.pathPattern="/svc"; rr.upstream="backend"; rr.stripPrefix=true;
    router->addRoute(rr);

    // 验证路由计数
    TEST_CHECK_MSG(router->routeCount() == 1, "route count 1, got " << router->routeCount());

        auto chain = std::make_shared<MwChain>();
    chain->use(MakeHealthCheckMiddleware("/healthz"));
    chain->use(MakeSecurityHeadersMiddleware());
    chain->use(MakeRouterMiddleware(router, upReg0));
    chain->use(MakeProxyMiddleware(2000, 5000));

    auto gw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    auto snap = std::make_shared<ConfigSnapshot>();
    snap->router = router;
    snap->upstreams = upReg0;
    snap->chain = chain;
    gw->setConfig(snap);
    gw->setRequestHandler([chain](GatewayConnection& c){ ReqCtx ctx(&c); chain->run(ctx); });
    gw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(GW_PORT)));
    gw->start();

    auto admin = std::make_shared<AdminServer>(gw.get(), &iom, &iom);
    admin->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(ADMIN_PORT)));
    admin->start();
    usleep(200*1000);

    // 配置的路由命中 → 转发 + 剥前缀 + 安全头
    {
        std::string resp = ex(GW_PORT, "GET /svc/ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find("200")!=std::string::npos, "200: " << resp.substr(0,30));
        TEST_CHECK_MSG(resp.find("OK:/ping")!=std::string::npos, "stripped path forwarded: " << resp);
        TEST_CHECK_MSG(resp.find("Content-Security-Policy")!=std::string::npos, "CSP present");
    }
    // 健康检查仍工作
    {
        std::string resp = ex(GW_PORT, "GET /healthz HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find("200")!=std::string::npos, "health 200");
    }
    // 未配置路由 → 404
    {
        std::string resp = ex(GW_PORT, "GET /unknown HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find("404")!=std::string::npos, "unconfigured route 404");
    }
    // Admin routes 输出详情，metrics 输出 Prometheus 文本
    {
        std::string resp = ex(ADMIN_PORT, "GET /routes HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find("\"routes\"")!=std::string::npos, "routes payload: " << resp);
        TEST_CHECK_MSG(resp.find("\"name\":\"svc\"")!=std::string::npos, "route detail includes svc: " << resp);
        TEST_CHECK_MSG(resp.find("\"upstreams\"")!=std::string::npos, "upstream detail present: " << resp);
        TEST_CHECK_MSG(resp.find("\"lb\":\"round_robin\"")!=std::string::npos, "lb detail present: " << resp);
        TEST_CHECK_MSG(resp.find("\"pool\":{\"idle\"")!=std::string::npos, "pool detail present: " << resp);
        TEST_CHECK_MSG(resp.find("\"circuit_breaker\"")!=std::string::npos, "circuit config present: " << resp);
        TEST_CHECK_MSG(resp.find("\"healthy_threshold\"")!=std::string::npos, "health config present: " << resp);
    }
    {
        std::string resp = ex(ADMIN_PORT, "GET /metrics HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find("gateway_requests_total")!=std::string::npos, "metrics text: " << resp);
        TEST_CHECK_MSG(resp.find("gateway_route_requests_total")!=std::string::npos, "route metrics text: " << resp);
        TEST_CHECK_MSG(resp.find("gateway_upstream_acquire_fail_total")!=std::string::npos,
                       "upstream acquire metric present: " << resp);
    }
    {
        std::string resp = ex(ADMIN_PORT, "GET /stats HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find("\"upstream_acquire_fail\"")!=std::string::npos,
                       "stats upstream acquire field present: " << resp);
        TEST_CHECK_MSG(resp.find("\"no_healthy_endpoint\"")!=std::string::npos,
                       "stats no healthy endpoint field present: " << resp);
    }

    admin->stop(); gw->stop(); up->stop();
    usleep(200*1000);
    return TEST_SUMMARY();
}
