#include "test_util.h"
#include "middlewares/waf.h"
#include "report.h"
#include "conn.h"
#include "ctx.h"
#include "gateway.h"
#include "mw.h"
#include "reactor.h"
#include "http_msg.h"
#include "metrics.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>

using namespace bronx;
using namespace bronx::gateway;
using namespace bronx::ipban;

// 发一个自定义请求行 收响应首行判状态。path 直接拼进请求行, 用来塞攻击串。
static std::string sendReq(uint16_t port, const std::string& reqLine,
                           const std::string& ua = "curl/8",
                           const std::string& moreHeaders = "") {
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
    std::string req = reqLine + " HTTP/1.1\r\nHost: x\r\nUser-Agent: " + ua
        + "\r\n" + moreHeaders + "Connection: close\r\n\r\n";
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

// 末端 200, 让过了 waf 的请求有个响应
static Middleware::ptr terminal200() {
    return std::make_shared<FuncMiddleware>(
        [](ReqCtx& ctx, const NextFn&) {
            auto rsp = std::make_shared<GwResponse>();
            rsp->setStatus(200);
            ctx.setResponse(rsp);
            ctx.setResponseBody("ok\n");
            ctx.setHandled(true);
            if(ctx.connection()) ctx.connection()->sendResponse(rsp, "ok\n");
        }, "t200");
}

static bool statusIs(uint16_t port, const std::string& reqLine, int want,
                     const std::string& ua = "curl/8",
                     const std::string& moreHeaders = "") {
    std::string resp = sendReq(port, reqLine, ua, moreHeaders);
    std::string w = "HTTP/1.1 " + std::to_string(want);
    return resp.compare(0, w.size(), w) == 0;
}

int main() {
    uint16_t port = 28100 + (::getpid() % 20000);
    BxIoManager iom(2, "waf-test");

    // waf on 挡住攻击
    {
        WafCfg cfg; cfg.on = true;
        auto reporter = std::make_shared<Reporter>(ReportOpts{});
        auto chain = std::make_shared<MwChain>();
        chain->use(MakeWaf(cfg, reporter, {}));
        chain->use(terminal200());

        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler([chain](GatewayConnection& conn) {
            ReqCtx ctx(&conn);
            chain->run(ctx);
        });
        TEST_CHECK(server->bind(BxAddress::LookupAny("127.0.0.1:" + std::to_string(port))));
        server->start();
        usleep(100 * 1000);

        auto before = GatewayMetrics::instance().snapshot();

        TEST_CHECK(statusIs(port, "GET /api/users", 200));
        TEST_CHECK(statusIs(port, "GET /api?id=1%20union%20select%20x", 403));
        TEST_CHECK(statusIs(port, "GET /static/../../etc/passwd", 403));
        TEST_CHECK(statusIs(port, "GET /static/%2e%2e%2fetc%2fpasswd", 403));
        TEST_CHECK(statusIs(port, "GET /static/%252e%252e%252fetc%252fpasswd", 403));
        TEST_CHECK(statusIs(port, "GET /search?q=union+select", 403));
        TEST_CHECK(statusIs(port, "GET /search?q=union2select", 200));
        TEST_CHECK(statusIs(port, "GET /", 403, "sqlmap/1.5"));
        TEST_CHECK(statusIs(port, "GET /", 403, "sqlmap/1.5",
                            "User-Agent: curl/8\r\n"));
        TEST_CHECK(statusIs(port, "GET /home", 200, "Mozilla/5.0"));
        auto after = GatewayMetrics::instance().snapshot();
        TEST_CHECK_EQ(after.wafByRule[0] - before.wafByRule[0], 1u);
        TEST_CHECK_EQ(after.wafByRule[1] - before.wafByRule[1], 1u);
        TEST_CHECK_EQ(after.wafByRule[5] - before.wafByRule[5], 3u);
        TEST_CHECK_EQ(after.wafByRule[7] - before.wafByRule[7], 2u);

        server->stop();
        reporter->stop();
        usleep(50 * 1000);
    }

    // waf off 攻击串也放行
    {
        WafCfg cfg; cfg.on = false;
        auto chain = std::make_shared<MwChain>();
        chain->use(MakeWaf(cfg, nullptr, {}));
        chain->use(terminal200());

        auto server = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
        server->setRequestHandler([chain](GatewayConnection& conn) {
            ReqCtx ctx(&conn);
            chain->run(ctx);
        });
        uint16_t port2 = port + 1;
        TEST_CHECK(server->bind(BxAddress::LookupAny("127.0.0.1:" + std::to_string(port2))));
        server->start();
        usleep(100 * 1000);

        TEST_CHECK(statusIs(port2, "GET /api?id=1%20union%20select%20x", 200));

        server->stop();
        usleep(50 * 1000);
    }

    return TEST_SUMMARY();
}
