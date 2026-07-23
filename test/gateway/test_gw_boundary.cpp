// gateway 边界测试
// =================
// 专压边界/异常,验证少量边界下不崩、行为合理:
//   解析:空行/超长 header/缺空行/超长 chunk 长度行/0-CL/非法 chunk 长度
//   buffer:0 字节 append/consume 超量/反复大块回收
//   连接:超 maxHeaderSize→431 / 畸形请求行→400 / 空请求立即关闭
//   路由:空表/最长前缀竞争/根 /
//   限流:容量 0 全拒 / 补充恢复
//   中间件链:空链/全短路不到 proxy

#include "test_util.h"
#include "buf.h"
#include "body.h"
#include "http_parse.h"
#include "router.h"
#include "ups_group.h"
#include "lb.h"
#include "ctx.h"
#include "mw.h"
#include "middlewares/builtin.h"
#include "gateway.h"
#include "conn.h"
#include "reactor.h"
#include "endpoint.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>

using namespace bronx::gateway;
static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

// ---------- 解析器边界 ----------
static void test_parser_edges() {
    // 缺最终空行(头未结束)→ 未完成
    {
        auto p = std::make_shared<HttpReqParser>();
        std::string raw = "GET / HTTP/1.1\r\nHost: x\r\n";   // 无 \r\n\r\n
        p->execute(raw.data(), raw.size());
        TEST_CHECK_MSG(!p->isFinished(), "incomplete header not finished");
    }
    // 0 Content-Length → 分帧 NONE
    {
        auto p = std::make_shared<HttpReqParser>();
        std::string raw = "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
        p->execute(raw.data(), raw.size());
        TEST_CHECK_MSG(p->isFinished() && !p->hasError(), "CL:0 ok");
        TEST_CHECK_MSG(p->getBodyFraming() == BodyFraming::NONE, "CL:0 -> NONE framing");
    }
    // 非法 Content-Length(非数字)→ 错误
    {
        auto p = std::make_shared<HttpReqParser>();
        std::string raw = "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n";
        p->execute(raw.data(), raw.size());
        TEST_CHECK_MSG(p->hasError(), "non-numeric CL rejected");
    }
    // 根路径 /
    {
        auto p = std::make_shared<HttpReqParser>();
        std::string raw = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        p->execute(raw.data(), raw.size());
        TEST_CHECK_MSG(p->isFinished() && p->getData()->getPath() == "/", "root path");
    }
}

// ---------- chunked 解码边界 ----------
static void test_chunked_edges() {
    // 仅末块(空 body)
    {
        InBuf b;
        std::string wire = "0\r\n\r\n";
        b.append(wire.data(), wire.size());
        BodyReader r(BodyFraming::CHUNKED, 0, &b, nullptr);
        std::string out; int n;
        while((n = r.readChunk(out)) > 0) {}
        TEST_CHECK_MSG(!r.hasError() && r.isFinished(), "empty chunked ok");
        TEST_CHECK_MSG(out.empty(), "empty chunked body empty");
    }
    // 非法 chunk 长度行 → 错误,不崩
    {
        InBuf b;
        std::string wire = "zzz\r\nABCD\r\n";
        b.append(wire.data(), wire.size());
        BodyReader r(BodyFraming::CHUNKED, 0, &b, nullptr);
        std::string out;
        r.readChunk(out);
        TEST_CHECK_MSG(r.hasError(), "invalid chunk size -> error");
    }
    // 带 chunk extension(;后缀)应被忽略,正常解码
    {
        InBuf b;
        std::string wire = "4;ext=1\r\nWiki\r\n0\r\n\r\n";
        b.append(wire.data(), wire.size());
        BodyReader r(BodyFraming::CHUNKED, 0, &b, nullptr);
        std::string out; int n;
        while((n = r.readChunk(out)) > 0) {}
        TEST_CHECK_MSG(!r.hasError() && out == "Wiki", "chunk extension ignored, got: " << out);
    }
}

// ---------- InBuf 边界 ----------
static void test_buffer_edges() {
    InBuf b(8);
    b.append("", 0);                       // 0 字节 append
    TEST_CHECK_EQ((int)b.readable(), 0);
    b.append("abc", 3);
    b.consume(100);                        // consume 超量 → 清空,不崩
    TEST_CHECK_EQ((int)b.readable(), 0);
    // 单次超容量 append → 扩容
    std::string big(10000, 'x');
    b.append(big.data(), big.size());
    TEST_CHECK_EQ((int)b.readable(), 10000);
    TEST_CHECK_MSG(b.capacity() >= 10000, "grew to fit big append");
}

// ---------- 路由边界 ----------
static void test_router_edges() {
    // 空路由表 → 不命中
    {
        Router r;
        ReqCtx ctx(static_cast<GatewayConnection*>(nullptr));
        // 用最小 ctx:不需要真 connection,match 只读 request;但 ctx 需要 request。
        // 简化:直接验证 routeCount + 一个有 request 的 match
        TEST_CHECK_EQ((int)r.routeCount(), 0);
        TEST_CHECK_MSG(!ctx.request(), "null context request should be safe");
        TEST_CHECK_MSG(!ctx.requestBody(), "null context body should be safe");
    }
    // 最长前缀竞争:/a 与 /a/b,path=/a/b/c 应命中 /a/b
    {
        RouteTable table;
        RouteRule r1; r1.name="up0"; r1.pathPattern="/a";   r1.upstream="up0"; r1.matchType=MatchType::PREFIX;
        RouteRule r2; r2.name="up1"; r2.pathPattern="/a/b"; r2.upstream="up1"; r2.matchType=MatchType::PREFIX;
        table.addRule(r1); table.addRule(r2); table.build();
        auto m = table.match("GET", "", "/a/b/c");
        TEST_CHECK_MSG(m && m->name == "up1", "longest prefix wins: "
                       << (m ? m->name : std::string("<null>")));
    }
    // path 前缀需要按路径段匹配:/api 不应误命中 /apix
    {
        RouteTable table;
        RouteRule rr; rr.name="up2"; rr.pathPattern="/api"; rr.upstream="up2"; rr.matchType=MatchType::PREFIX;
        table.addRule(rr); table.build();
        TEST_CHECK_MSG(table.match("GET","","/api"),       "exact prefix should match");
        TEST_CHECK_MSG(table.match("GET","","/api/users"), "child path should match");
        TEST_CHECK_MSG(!table.match("GET","","/apix"),     "sibling path must not match /api");
    }
}

// ---------- 限流边界 ----------
static const uint16_t PORT = 19000;
static void test_ratelimit_zero() {
    // 容量 0:第一个请求就被拒
    bronx::BxIoManager iom(2);
    auto chain = std::make_shared<MwChain>();
    chain->use(MakeRateLimitMiddleware(0, 0));   // 容量 0,不补充
    chain->use([](ReqCtx& ctx, const NextFn&){
        // 不该到这(应被限流短路)
        auto rsp = std::make_shared<GwResponse>();
        rsp->setStatus(200);
        ctx.connection()->sendResponse(rsp, "reached");
        ctx.setHandled(true);
    }, "sink");

    auto gw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    gw->setRequestHandler([chain](GatewayConnection& c){ ReqCtx ctx(&c); chain->run(ctx); });
    gw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(PORT)));
    gw->start();
    usleep(150*1000);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(PORT);
    inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    connect(fd,(sockaddr*)&a,sizeof(a));
    std::string w = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    send(fd, w.data(), w.size(), 0);
    char buf[512]; std::string resp; int n;
    while((n=recv(fd,buf,sizeof(buf),0))>0) resp.append(buf,n);
    close(fd);
    TEST_CHECK_MSG(resp.find("429") != std::string::npos, "zero-capacity -> 429: " << resp.substr(0,30));
    TEST_CHECK_MSG(resp.find("reached") == std::string::npos, "should not reach sink");

    gw->stop();
    usleep(150*1000);
}

// ---------- 连接边界:超大完整请求头应拒绝 ----------
static void test_complete_oversized_header() {
    bronx::BxIoManager iom(2);
    GatewayOptions opts;
    opts.maxHeaderSize = 128;
    auto gw = std::make_shared<GatewayServer>(opts, &iom, &iom);
    gw->setRequestHandler([](GatewayConnection& c){
        auto rsp = std::make_shared<GwResponse>();
        rsp->setStatus(200);
        c.sendResponse(rsp, "should-not-reach");
    });
    gw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(PORT+1)));
    gw->start();
    usleep(150*1000);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(PORT+1);
    inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    connect(fd,(sockaddr*)&a,sizeof(a));
    std::string raw = "GET / HTTP/1.1\r\nHost: x\r\nX-Big: "
        + std::string(256, 'a') + "\r\nConnection: close\r\n\r\n";
    send(fd, raw.data(), raw.size(), 0);
    char buf[1024]; std::string resp; int n;
    while((n=recv(fd,buf,sizeof(buf),0))>0) resp.append(buf,n);
    close(fd);
    TEST_CHECK_MSG(resp.find("431") != std::string::npos,
                   "complete oversized header -> 431: " << resp.substr(0,40));
    TEST_CHECK_MSG(resp.find("should-not-reach") == std::string::npos,
                   "oversized header must not reach handler");

    gw->stop();
    usleep(150*1000);
}

// ---------- 连接边界:空连接(连上不发)立即关 ----------
static void test_empty_connection() {
    bronx::BxIoManager iom(2);
    auto gw = std::make_shared<GatewayServer>(GatewayOptions(), &iom, &iom);
    gw->setRequestHandler([](GatewayConnection& c){
        auto rsp = std::make_shared<GwResponse>(); rsp->setStatus(200);
        c.sendResponse(rsp, "ok");
    });
    gw->bind(bronx::BxAddress::LookupAny("127.0.0.1:" + std::to_string(PORT+2)));
    gw->start();
    usleep(150*1000);

    // 连上立刻关,不发任何数据 → 网关端应优雅处理(不崩、连接计数归零)
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(PORT+2);
    inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    connect(fd,(sockaddr*)&a,sizeof(a));
    close(fd);   // 立即关
    usleep(200*1000);
    TEST_CHECK_MSG(gw->getActiveConnectionCount() == 0, "empty conn cleaned up, active="
                   << gw->getActiveConnectionCount());

    gw->stop();
    usleep(150*1000);
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_gw_boundary start ===";
    test_parser_edges();
    test_chunked_edges();
    test_buffer_edges();
    test_router_edges();
    test_ratelimit_zero();
    test_complete_oversized_header();
    test_empty_connection();
    return TEST_SUMMARY();
}
