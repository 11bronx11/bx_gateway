#include "test_util.h"
#include "banmw.h"
#include "daemon.h"
#include "guard.h"
#include "middlewares/builtin.h"
#include "pull.h"
#include "report.h"
#include "middlewares/waf.h"
#include "conn.h"
#include "ctx.h"
#include "gateway.h"
#include "http_msg.h"
#include "mw.h"
#include "reactor.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>

using namespace bronx;
using namespace bronx::gateway;
using namespace bronx::ipban;

static Ip ip(const char* s) {
    Ip out;
    parseCidr(s, out);
    return out;
}

static std::string get(uint16_t port, const std::string& path, const std::string& xff) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return "";
    timeval tv{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "";
    }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: "
        + xff + "\r\nConnection: close\r\n\r\n";
    if(::send(fd, req.data(), req.size(), MSG_NOSIGNAL) != (ssize_t)req.size()) {
        ::close(fd);
        return "";
    }
    std::string out;
    char buf[1024];
    for(;;) {
        int n = ::recv(fd, buf, sizeof(buf), 0);
        if(n <= 0) break;
        out.append(buf, n);
    }
    ::close(fd);
    return out;
}

static bool status(const std::string& rsp, int want) {
    std::string head = "HTTP/1.1 " + std::to_string(want);
    return rsp.compare(0, head.size(), head) == 0;
}

static bool waitDeny(const Guard::ptr& guard, const Ip& addr, Src src) {
    for(int i = 0; i < 100; ++i) {
        auto d = guard->eval(addr);
        if(d.action == Act::DENY && d.src == src) return true;
        usleep(50 * 1000);
    }
    auto d = guard->eval(addr);
    return d.action == Act::DENY && d.src == src;
}

int main() {
    std::string base = "/tmp/bronx_ipban_risk_" + std::to_string(::getpid());
    DaemonOpts daemonOpts;
    daemonOpts.submitPath = base + "_submit.sock";
    daemonOpts.subscribePath = base + "_sub.sock";
    daemonOpts.expiryTickMs = 100;

    BxIoManager daemonIom(1, "risk-daemon");
    BxIoManager gatewayIom(2, "risk-gateway");
    Daemon daemon(&daemonIom, daemonOpts);
    auto guard = std::make_shared<Guard>();
    SyncOpts syncOpts;
    syncOpts.subscribePath = daemonOpts.subscribePath;
    syncOpts.instanceId = "risk-test";
    auto sync = std::make_shared<SyncClient>(guard, syncOpts);
    ReportOpts reportOpts;
    reportOpts.submitPath = daemonOpts.submitPath;
    auto reporter = std::make_shared<Reporter>(reportOpts);

    daemonIom.post([&]() { TEST_CHECK(daemon.start()); });
    gatewayIom.post([&]() {
        sync->start(&gatewayIom);
        reporter->start(&gatewayIom);
    });

    uint16_t port = 28200 + (::getpid() % 20000);
    std::vector<Ip> trusted = {ip("127.0.0.1")};
    auto route = std::make_shared<RouteRule>();
    route->name = "limited";
    route->pathPattern = "/";
    route->rateLimitEnabled = true;
    route->rateLimitCapacity = 1;
    route->rateLimitRefillPerSec = 0;
    route->rateLimitKey = "ip";
    RateBanCfg rateCfg;
    rateCfg.on = true;
    rateCfg.hits = 2;
    rateCfg.windowMs = 60000;
    rateCfg.banMs = 60000;

    auto chain = std::make_shared<MwChain>();
    chain->use(MakeBanMiddleware(guard, trusted));
    chain->use(MakeWaf(WafCfg{true, 60000}, reporter, trusted));
    chain->use([route](ReqCtx& ctx, const NextFn& next) {
        ctx.route().matched = true;
        ctx.route().rule = route;
        ctx.route().routeKey = route->name;
        next();
    }, "route");
    chain->use(MakePerRouteRateLimitMiddleware(trusted, reporter, rateCfg));
    chain->use([](ReqCtx& ctx, const NextFn&) {
        auto rsp = std::make_shared<GwResponse>();
        rsp->setStatus(200);
        ctx.setResponse(rsp);
        ctx.setHandled(true);
        ctx.connection()->sendResponse(rsp, "ok\n");
    }, "done");

    auto server = std::make_shared<GatewayServer>(GatewayOptions(), &gatewayIom, &gatewayIom);
    server->setRequestHandler([chain](GatewayConnection& conn) {
        ReqCtx ctx(&conn);
        chain->run(ctx);
    });
    TEST_CHECK(server->bind(BxAddress::LookupAny("127.0.0.1:" + std::to_string(port))));
    server->start();
    usleep(200 * 1000);

    Ip wafIp = ip("198.51.100.10");
    TEST_CHECK(status(get(port, "/?q=union%20select", wafIp.toString()), 403));
    TEST_CHECK_MSG(waitDeny(guard, wafIp, Src::WAF), "waf risk must reach guard");
    TEST_CHECK(status(get(port, "/ok", wafIp.toString()), 403));

    Ip rateIp = ip("198.51.100.20");
    TEST_CHECK(status(get(port, "/ok", rateIp.toString()), 200));
    TEST_CHECK(status(get(port, "/ok", rateIp.toString()), 429));
    TEST_CHECK(status(get(port, "/ok", rateIp.toString()), 429));
    TEST_CHECK_MSG(waitDeny(guard, rateIp, Src::RATE), "rate risk must reach guard");
    TEST_CHECK(status(get(port, "/ok", rateIp.toString()), 403));

    server->stop();
    sync->stop();
    reporter->stop();
    daemon.stop();
    gatewayIom.stop();
    daemonIom.stop();
    ::unlink(daemonOpts.submitPath.c_str());
    ::unlink(daemonOpts.subscribePath.c_str());
    return TEST_SUMMARY();
}
