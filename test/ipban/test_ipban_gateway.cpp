#include "test_util.h"
#include "banmw.h"
#include "conn.h"
#include "conf.h"
#include "ctx.h"
#include "endpoint.h"
#include "gateway.h"
#include "guard.h"
#include "metrics.h"
#include "middlewares/builtin.h"
#include "mw.h"
#include "reactor.h"
#include "router.h"
#include "log.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <string>
#include <thread>

using namespace bronx;
using namespace bronx::gateway;
using namespace bronx::ipban;

static Ip cidr(const char* s) {
    Ip ip;
    parseCidr(s, ip);
    return ip;
}

static std::string requestOnce(uint16_t port, const std::string& req) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return "";
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "";
    }
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

static std::string getOnce(uint16_t port, const std::string& xff = "8.8.8.8", bool keepAlive = true) {
    const std::string req = "GET / HTTP/1.1\r\nHost: x\r\n"
        "X-Forwarded-For: " + xff + "\r\nConnection: "
        + (keepAlive ? "keep-alive" : "close") + "\r\n\r\n";
    return requestOnce(port, req);
}

class WsOnce {
public:
    WsOnce() {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        TEST_CHECK(::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        TEST_CHECK(::listen(m_fd, 1) == 0);
        socklen_t len = sizeof(addr);
        TEST_CHECK(::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port = ntohs(addr.sin_port);
        m_worker = std::thread([this] { serve(); });
    }

    ~WsOnce() {
        if(m_fd >= 0) {
            ::shutdown(m_fd, SHUT_RDWR);
            ::close(m_fd);
        }
        if(m_worker.joinable()) m_worker.join();
    }

    uint16_t port = 0;

private:
    void serve() {
        int c = ::accept(m_fd, nullptr, nullptr);
        if(c < 0) return;
        char buf[1024];
        std::string head;
        while(head.find("\r\n\r\n") == std::string::npos) {
            int n = ::recv(c, buf, sizeof(buf), 0);
            if(n <= 0) break;
            head.append(buf, n);
        }
        const std::string rsp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
        ::send(c, rsp.data(), rsp.size(), MSG_NOSIGNAL);
        ::shutdown(c, SHUT_RDWR);
        ::close(c);
    }

    int m_fd = -1;
    std::thread m_worker;
};

static std::string wsOnce(uint16_t port, const std::string& key = "") {
    std::string req =
        "GET /ws HTTP/1.1\r\n"
        "Host: x\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n";
    if(!key.empty()) req += "X-API-Key: " + key + "\r\n";
    req += "\r\n";
    return requestOnce(port, req);
}

int main() {
    uint16_t port = 28000 + (::getpid() % 20000);
    BxIoManager iom(2, "ipban-gw");
    {
        auto guard = std::make_shared<Guard>();
        Rule rule;
        rule.id = "loopback";
        TEST_CHECK(parseCidr("127.0.0.1", rule.ip));
        rule.action = Act::DENY;
        rule.src = Src::RATE;
        rule.priority = rulePriority(rule.src, rule.action);
        guard->setRemoteEnabled(true);
        guard->applyRemote(1, 1, {rule});

        YAML::Node config = YAML::Load("ip_filter:\n  enabled: false\n  mode: denylist\n  cidrs: []\n");
        GatewayConfig::BuildSnapshotFromYaml(config, nullptr, guard);
        TEST_CHECK_MSG(guard->eval(rule.ip).action == Act::DENY,
                       "config reload must keep remote deny active");

        YAML::Node offAllow = YAML::Load(
            "ip_filter:\n"
            "  enabled: false\n"
            "  mode: allowlist\n"
            "  cidrs: []\n");
        GatewayConfig::BuildSnapshotFromYaml(offAllow, nullptr, guard);
        TEST_CHECK_MSG(guard->eval(cidr("9.9.9.9")).action == Act::ALLOW,
                       "disabled allowlist must not deny remote misses");

        YAML::Node goodStatic = YAML::Load(
            "ip_filter:\n"
            "  enabled: true\n"
            "  mode: denylist\n"
            "  cidrs: [1.2.3.4]\n");
        TEST_CHECK(GatewayConfig::BuildSnapshotFromYaml(goodStatic, nullptr, guard));
        TEST_CHECK(guard->eval(cidr("1.2.3.4")).action == Act::DENY);

        // 后段报错, 旧规则别动
        YAML::Node badLate = YAML::Load(
            "ip_filter:\n"
            "  enabled: true\n"
            "  mode: denylist\n"
            "  cidrs: [2.2.2.2]\n"
            "auth:\n"
            "  jwt:\n"
            "    enabled: true\n"
            "    leeway_sec: nope\n");
        bool threw = false;
        try {
            GatewayConfig::BuildSnapshotFromYaml(badLate, nullptr, guard);
        } catch(...) {
            threw = true;
        }
        TEST_CHECK(threw);
        TEST_CHECK(guard->eval(cidr("1.2.3.4")).action == Act::DENY);
        TEST_CHECK(guard->eval(cidr("2.2.2.2")).action == Act::ALLOW);

        YAML::Node badStatic = YAML::Load(
            "ip_filter:\n"
            "  enabled: true\n"
            "  mode: denylist\n"
            "  cidrs: [1.2.3.4, bad-ip]\n");
        TEST_CHECK_MSG(!GatewayConfig::BuildSnapshotFromYaml(badStatic, nullptr, guard),
                       "坏 cidr 应拒绝配置");
        TEST_CHECK_MSG(guard->eval(cidr("1.2.3.4")).action == Act::DENY,
                       "坏配置不能覆盖旧规则");

        YAML::Node disabledBad = YAML::Load(
            "ip_filter:\n"
            "  enabled: false\n"
            "  cidrs: [bad-ip]\n");
        TEST_CHECK_MSG(!GatewayConfig::BuildSnapshotFromYaml(disabledBad, nullptr, guard),
                       "关着的坏 cidr 也应拒绝配置");

        auto chain = std::make_shared<MwChain>();
        chain->use(MakeBanMiddleware(guard));
        chain->use([](ReqCtx& ctx, const NextFn&) {
            auto rsp = std::make_shared<GwResponse>();
            rsp->setStatus(200);
            ctx.connection()->sendResponse(rsp, "reached");
            ctx.setHandled(true);
        }, "sink");

        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler([chain](GatewayConnection& conn) {
            ReqCtx ctx(&conn);
            chain->run(ctx);
        });
        TEST_CHECK(server->bind(BxAddress::LookupAny("127.0.0.1:" + std::to_string(port))));
        server->start();
        usleep(150 * 1000);

        auto before = GatewayMetrics::instance().snapshot();
        std::string resp = getOnce(port);
        auto after = GatewayMetrics::instance().snapshot();
        TEST_CHECK_MSG(resp.find(" 403 ") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find("Connection: close") != std::string::npos, resp);
        TEST_CHECK(resp.find("reached") == std::string::npos);
        TEST_CHECK_EQ(after.ipbanDenied, before.ipbanDenied + 1);
        size_t denied = static_cast<size_t>(Src::RATE) * 2;
        TEST_CHECK_EQ(after.denied[denied], before.denied[denied] + 1);

        server->stop();
        usleep(100 * 1000);
    }

    // 场景二: 配了可信代理(127.0.0.1)后, 封的是 XFF 里的真实客户端 8.8.8.8, 不是代理本身。
    // 请求从 loopback 来(peer=127.0.0.1=可信代理), 带 XFF: 8.8.8.8 -> 解析出真凶 -> 403。
    {
        auto guard = std::make_shared<Guard>();
        Rule rule;
        rule.id = "realclient";
        TEST_CHECK(parseCidr("8.8.8.8", rule.ip));   // 封真实客户端, 不封 127.0.0.1
        rule.action = Act::DENY;
        rule.src = Src::RATE;
        rule.priority = rulePriority(rule.src, rule.action);
        guard->setRemoteEnabled(true);
        guard->applyRemote(1, 1, {rule});

        auto chain = std::make_shared<MwChain>();
        std::vector<Ip> trusted; trusted.push_back(Ip{}); parseCidr("127.0.0.1", trusted.back());
        chain->use(MakeBanMiddleware(guard, trusted));   // 127.0.0.1 是可信代理
        chain->use([](ReqCtx& ctx, const NextFn&) {
            auto rsp = std::make_shared<GwResponse>();
            rsp->setStatus(200);
            ctx.connection()->sendResponse(rsp, "reached");
            ctx.setHandled(true);
        }, "sink");

        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler([chain](GatewayConnection& conn) {
            ReqCtx ctx(&conn);
            chain->run(ctx);
        });
        uint16_t port2 = port + 1;
        TEST_CHECK(server->bind(BxAddress::LookupAny("127.0.0.1:" + std::to_string(port2))));
        server->start();
        usleep(150 * 1000);

        // getOnce 带的就是 XFF: 8.8.8.8, 经可信代理解析应封到它
        std::string resp = getOnce(port2);
        TEST_CHECK_MSG(resp.find(" 403 ") != std::string::npos, "可信代理后应封 XFF 真实客户端: " + resp);
        TEST_CHECK(resp.find("reached") == std::string::npos);

        resp = getOnce(port2, "8.8.8.8/24");
        TEST_CHECK_MSG(resp.find(" 403 ") != std::string::npos, "CIDR XFF 应直接拒绝: " + resp);

        server->stop();
        usleep(100 * 1000);
    }

    {
        auto guard = std::make_shared<Guard>();
        YAML::Node config = YAML::Load(
            "trusted_proxies: [127.0.0.1]\n"
            "ip_filter:\n"
            "  enabled: true\n"
            "  mode: denylist\n"
            "  cidrs: [8.8.8.8]\n"
            "ip_policy:\n"
            "  waf:\n"
            "    enabled: true\n");
        auto snap = GatewayConfig::BuildSnapshotFromYaml(config, nullptr, guard);
        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler([chain = snap->chain](GatewayConnection& conn) {
            ReqCtx ctx(&conn);
            chain->run(ctx);
        });
        uint16_t port3 = port + 2;
        TEST_CHECK(server->bind(BxAddress::LookupAny("127.0.0.1:" + std::to_string(port3))));
        server->start();
        usleep(150 * 1000);

        std::string resp = getOnce(port3);
        TEST_CHECK_MSG(resp.find(" 403 ") != std::string::npos, "配置里的可信代理应生效: " + resp);

        resp = requestOnce(port3,
            "GET /healthz HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: 8.8.8.8\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find(" 403 ") != std::string::npos, resp);

        resp = requestOnce(port3,
            "OPTIONS /x HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
            "Access-Control-Request-Method: GET\r\nX-Forwarded-For: 8.8.8.8\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find(" 403 ") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find("Access-Control-Allow-Origin: *") != std::string::npos, resp);

        resp = requestOnce(port3,
            "GET /healthz HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: 9.9.9.9\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find(" 200 ") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find("X-Request-Id:") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find("Content-Security-Policy:") != std::string::npos, resp);

        auto common = [](const std::string& v) {
            TEST_CHECK_MSG(v.find("X-Request-Id:") != std::string::npos, v);
            TEST_CHECK_MSG(v.find("Content-Security-Policy:") != std::string::npos, v);
        };

        MaintGate::instance().set(true);
        resp = requestOnce(port3,
            "GET /healthz HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: 9.9.9.9\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find(" 200 ") != std::string::npos, resp);
        common(resp);

        resp = requestOnce(port3,
            "GET /healthz HTTP/1.1\r\nHost: x\r\nUser-Agent: nikto\r\n"
            "X-Forwarded-For: 9.9.9.9\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find(" 200 ") != std::string::npos, resp);
        common(resp);

        resp = requestOnce(port3,
            "OPTIONS /../../etc/passwd HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
            "Access-Control-Request-Method: GET\r\nX-Forwarded-For: 9.9.9.9\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find(" 204 ") != std::string::npos, resp);
        TEST_CHECK_MSG(resp.find("Access-Control-Allow-Origin: *") != std::string::npos, resp);
        common(resp);

        resp = requestOnce(port3,
            "GET /../../etc/passwd HTTP/1.1\r\nHost: x\r\n"
            "X-Forwarded-For: 9.9.9.9\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find(" 503 ") != std::string::npos, resp);
        common(resp);

        MaintGate::instance().set(false);
        resp = requestOnce(port3,
            "GET /../../etc/passwd HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: 9.9.9.9\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find(" 403 ") != std::string::npos, resp);
        common(resp);

        resp = requestOnce(port3,
            "GET /miss HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: 9.9.9.9\r\nConnection: close\r\n\r\n");
        TEST_CHECK_MSG(resp.find(" 404 ") != std::string::npos, resp);
        common(resp);

        server->stop();
        usleep(100 * 1000);
    }

    {
        auto rule = std::make_shared<RouteRule>();
        rule->name = "ip-rate";
        rule->pathPattern = "/";
        rule->rateLimitEnabled = true;
        rule->rateLimitCapacity = 1;
        rule->rateLimitRefillPerSec = 0;
        rule->rateLimitKey = "ip";
        std::vector<Ip> trusted = {cidr("127.0.0.1")};
        auto chain = std::make_shared<MwChain>();
        chain->use([rule](ReqCtx& ctx, const NextFn& next) {
            ctx.route().matched = true;
            ctx.route().rule = rule;
            ctx.route().routeKey = rule->name;
            next();
        }, "route");
        chain->use(MakePerRouteRateLimitMiddleware(trusted, nullptr, {}, 2));
        chain->use([](ReqCtx& ctx, const NextFn&) {
            auto rsp = std::make_shared<GwResponse>();
            rsp->setStatus(200);
            ctx.setResponse(rsp);
            ctx.connection()->sendResponse(rsp, "reached");
            ctx.setHandled(true);
        }, "sink");

        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler([chain](GatewayConnection& conn) {
            ReqCtx ctx(&conn);
            chain->run(ctx);
        });
        uint16_t port4 = port + 3;
        TEST_CHECK(server->bind(BxAddress::LookupAny("127.0.0.1:" + std::to_string(port4))));
        server->start();
        usleep(150 * 1000);

        TEST_CHECK(getOnce(port4, "198.51.100.1", false).find(" 200 ") != std::string::npos);
        TEST_CHECK(getOnce(port4, "198.51.100.2", false).find(" 200 ") != std::string::npos);
        TEST_CHECK(getOnce(port4, "198.51.100.3", false).find(" 429 ") != std::string::npos);
        TEST_CHECK(getOnce(port4, "198.51.100.1", false).find(" 429 ") != std::string::npos);

        server->stop();
        usleep(100 * 1000);
    }

    {
        std::string logFile = "/tmp/bronx_ipban_access.log";
        ::unlink(logFile.c_str());
        auto sys = BRONX_LOG_NAME("system");
        auto appender = std::make_shared<BxFileLogAppender>(logFile);
        sys->addAppender(appender);
        sys->setLevel(BxLogLevel::INFO);

        std::vector<Ip> trusted = {cidr("127.0.0.1")};
        auto chain = std::make_shared<MwChain>();
        chain->use(MakeStructuredAccessLogMiddleware(trusted));
        chain->use([](ReqCtx& ctx, const NextFn&) {
            auto rsp = std::make_shared<GwResponse>();
            rsp->setStatus(200);
            ctx.setResponse(rsp);
            ctx.connection()->sendResponse(rsp, "reached");
            ctx.setHandled(true);
        }, "sink");

        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler([chain](GatewayConnection& conn) {
            ReqCtx ctx(&conn);
            chain->run(ctx);
        });
        uint16_t port5 = port + 4;
        TEST_CHECK(server->bind(BxAddress::LookupAny("127.0.0.1:" + std::to_string(port5))));
        server->start();
        usleep(150 * 1000);

        TEST_CHECK(getOnce(port5, "198.51.100.9", false).find(" 200 ") != std::string::npos);
        server->stop();
        appender->flush();
        usleep(100 * 1000);

        std::ifstream in(logFile);
        std::string line, all;
        while(std::getline(in, line)) all += line;
        TEST_CHECK_MSG(all.find("\"ip\":\"198.51.100.9\"") != std::string::npos, all);
        sys->delAppender(appender);
        ::unlink(logFile.c_str());
    }

    {
        WsOnce upstream;
        std::string yaml =
            "upstreams:\n"
            "  - name: ws\n"
            "    endpoints:\n"
            "      - host: 127.0.0.1\n"
            "        port: " + std::to_string(upstream.port) + "\n"
            "routes:\n"
            "  - name: secure_ws\n"
            "    match_type: exact\n"
            "    path: /ws\n"
            "    methods: [GET]\n"
            "    upstream: ws\n"
            "    auth: api_key\n"
            "    rate_limit:\n"
            "      enabled: true\n"
            "      capacity: 1\n"
            "      refill_per_sec: 0\n"
            "      key: ip\n"
            "auth:\n"
            "  api_key:\n"
            "    keys: [ws-key]\n";
        auto snap = GatewayConfig::BuildSnapshotFromYaml(YAML::Load(yaml));
        TEST_CHECK(snap);
        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler([chain = snap->chain](GatewayConnection& conn) {
            ReqCtx ctx(&conn);
            chain->run(ctx);
        });
        uint16_t port6 = port + 5;
        TEST_CHECK(server->bind(BxAddress::LookupAny("127.0.0.1:" + std::to_string(port6))));
        server->start();
        usleep(150 * 1000);

        TEST_CHECK_MSG(wsOnce(port6, "ws-key").find(" 101 ") != std::string::npos, "ws before proxy");
        TEST_CHECK_MSG(wsOnce(port6).find(" 401 ") != std::string::npos, "auth before rate");
        TEST_CHECK_MSG(wsOnce(port6, "ws-key").find(" 429 ") != std::string::npos, "rate before ws");

        server->stop();
        usleep(100 * 1000);
    }
    return TEST_SUMMARY();
}
