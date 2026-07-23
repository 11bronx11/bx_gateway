#include "upstream.h"
#include "mw.h"
#include "str.h"
#include "router.h"
#include "ups_group.h"
#include "metrics.h"
#include "conn.h"
#include "http_parse.h"
#include "body.h"
#include "buf.h"
#include "log.h"
#include "io.h"
#include "net_socket.h"
#include "endpoint.h"
#include "util.h"
#include <strings.h>
#include <cerrno>
#include <cctype>
#include <algorithm>

namespace bronx {
namespace gateway {

static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

static bool is_hop_hdr(const std::string& key) {
    static const char* hop[] = {
        "Connection", "Keep-Alive", "Proxy-Authenticate", "Proxy-Authorization", "Proxy-Connection",
        "TE", "Trailer", "Trailers", "Transfer-Encoding", "Upgrade"
    };
    for(auto h : hop)
        if(strcasecmp(key.c_str(), h) == 0) return true;
    return false;
}

static bool is_expect_hdr(const std::string& key) {
    return strcasecmp(key.c_str(), "Expect") == 0;
}

static bool only_token(const std::string& value, const char* wanted) {
    bool saw = false;
    size_t pos = 0;
    while(pos <= value.size()) {
        size_t comma = value.find(',', pos);
        std::string part = trim_ascii(value.substr(pos, comma == std::string::npos
                                                   ? std::string::npos : comma - pos));
        if(part.empty() || strcasecmp(part.c_str(), wanted) != 0) return false;
        saw = true;
        if(comma == std::string::npos) break;
        pos = comma + 1;
    }
    return saw;
}

static bool can_retry(HttpMethod method) {
    return method == HttpMethod::GET || method == HttpMethod::HEAD
        || method == HttpMethod::OPTIONS;
}

// 客户端/上游不可信任的转发元数据头, 网关自己重写, 不透传
static bool is_fwd_hdr(const std::string& key) {
    static const char* fwd[] = {
        "X-Forwarded-For", "X-Real-IP", "X-Forwarded-Host", "X-Forwarded-Proto"
    };
    for(auto h : fwd)
        if(strcasecmp(key.c_str(), h) == 0) return true;
    return false;
}

static bool resp_no_body(HttpMethod m, int s) {
    return m == HttpMethod::HEAD || (s >= 100 && s < 200) || s == 204 || s == 304;
}

static ForwardResult read_up_head(InBuf& ubuf,
                                  const bronx::BxSocket::ptr& usock,
                                  HttpRespParser::ptr& parser,
                                  GwResponse::ptr& rsp,
                                  uint64_t dueMs = 0,
                                  uint64_t recvMs = 0) {
    size_t headLen = 0;
    static constexpr size_t kMaxHead = 64 * 1024;
    while((headLen = ubuf.findHeaderEnd()) == 0) {
        if(ubuf.readable() >= kMaxHead) {
            return ForwardResult::BAD_RESPONSE;
        }
        if(dueMs) {
            uint64_t wait = std::min(recvMs, leftMs(dueMs));
            if(wait == 0) return ForwardResult::UPSTREAM_TIMEOUT;
            usock->setRecvTimeout(wait);
        }
        int rt = ubuf.fill(usock.get());
        if(rt == 0) {
            return ForwardResult::BAD_RESPONSE;
        }
        if(rt < 0) {
            return errno == ETIMEDOUT ? ForwardResult::UPSTREAM_TIMEOUT : ForwardResult::BAD_RESPONSE;
        }
    }
    if(headLen > kMaxHead) return ForwardResult::BAD_RESPONSE;
    parser = std::make_shared<HttpRespParser>();
    std::string headBuf(ubuf.peek(), headLen);
    parser->execute(headBuf.c_str(), headLen);
    if(parser->hasError() || !parser->isFinished()) {
        return ForwardResult::BAD_RESPONSE;
    }
    rsp = parser->getData();
    ubuf.consume(headLen);
    return ForwardResult::OK;
}

static HeaderList eff_headers(const GwRequest::ptr& req) {
    if(!req) return {};
    if(!req->getHeaderList().empty()) return req->getHeaderList();
    HeaderList out;
    for(const auto& h : req->getHeaders()) {
        out.emplace_back(h.first, h.second);
    }
    return out;
}

static HeaderList eff_headers(const GwResponse::ptr& rsp) {
    if(!rsp) return {};
    if(!rsp->getHeaderList().empty()) return rsp->getHeaderList();
    HeaderList out;
    for(const auto& h : rsp->getHeaders()) {
        out.emplace_back(h.first, h.second);
    }
    return out;
}

static void clean_resp_hdrs(const GwResponse::ptr& rsp) {
    HeaderList headers = eff_headers(rsp);
    std::string connection = join_hdr_vals(headers, "Connection");
    for(const auto& h : headers) {
        if(is_hop_hdr(h.first) || hdr_has_token(connection, h.first) || is_fwd_hdr(h.first)) {
            rsp->delHeader(h.first);
        }
    }
}

static std::string join_prefix(const std::string& prefix, const std::string& path) {
    if(prefix.empty()) return path.empty() ? "/" : path;
    std::string suffix = path.empty() ? "/" : path;
    if(prefix == "/") {
        return suffix[0] == '/' ? suffix : "/" + suffix;
    }
    if(prefix.back() == '/' && suffix[0] == '/') {
        return prefix + suffix.substr(1);
    }
    if(prefix.back() != '/' && suffix[0] != '/') {
        return prefix + "/" + suffix;
    }
    return prefix + suffix;
}

// 构造发往上游的请求头，含 M9 header rewrite, path strip/rewrite, XFF 注入
static std::string build_req_head(ReqCtx& ctx,
                                  const std::string& upstreamHost,
                                  uint32_t upstreamPort,
                                  const std::vector<bronx::ipban::Ip>& trusted) {
    auto req  = ctx.request();
    const auto& route = ctx.route();
    const RouteRule* rule = route.rule.get();

    std::string clientIp;
    std::string proto = "http";
    std::string origHost = req->getHeader("Host");
    bool clientOk = loadClientAddr(ctx, trusted);
    if(clientOk) {
        clientIp = ctx.clientAddr().client.toString();
    }
    if(clientOk && bronx::ipban::isTrustedProxy(ctx.clientAddr().peer, trusted)) {
        std::string got = trim_ascii(req->getHeader("X-Forwarded-Proto"));
        if(strcasecmp(got.c_str(), "https") == 0) proto = "https";
        else if(strcasecmp(got.c_str(), "http") == 0) proto = "http";
    }

    GwRequest up;
    up.setMethodRaw(req->getMethodRaw().empty()
                    ? HttpMethodToString(req->getMethod()) : req->getMethodRaw());
    up.setMethod(req->getMethod());
    up.setVersion(0x11);

    // M9 path rewrite：先剥前缀，再加 rewritePrefix
    std::string path = req->getPath();
    if(rule && rule->stripPrefix && !rule->pathPattern.empty()) {
        if(path.compare(0, rule->pathPattern.size(), rule->pathPattern) == 0) {
            path = path.substr(rule->pathPattern.size());
            if(path.empty() || path[0] != '/') path = "/" + path;
        }
    }
    if(rule && !rule->rewritePrefix.empty()) {
        path = join_prefix(rule->rewritePrefix, path);
    }
    up.setPath(path);
    up.setQuery(req->getQuery());

    // 透传头: 剥逐跳头 + 剥 XFF 类(客户端伪造的, 下面统一重写)
    HeaderList reqHeaders = eff_headers(req);
    std::string connectionHeader = join_hdr_vals(reqHeaders, "Connection");
    for(const auto& h : reqHeaders) {
        if(is_hop_hdr(h.first) || hdr_has_token(connectionHeader, h.first)
           || is_fwd_hdr(h.first) || is_expect_hdr(h.first)) continue;
        up.addHeader(h.first, h.second);
    }

    // M9 header rewrite：先 set，再 remove
    if(rule) {
        std::string setConnection;
        for(const auto& kv : rule->reqHeaderSet) {
            if(strcasecmp(kv.first.c_str(), "Connection") == 0) {
                if(!setConnection.empty()) setConnection += ",";
                setConnection += kv.second;
            }
        }
        for(const auto& kv : rule->reqHeaderSet) {
            if(is_hop_hdr(kv.first) || hdr_has_token(setConnection, kv.first)
               || is_fwd_hdr(kv.first) || is_expect_hdr(kv.first)) continue;
            up.setHeader(kv.first, kv.second);
        }
        for(const auto& k : rule->reqHeaderRemove)
            up.delHeader(k);
    }

    // 注入网关侧算出的转发元数据头, 覆盖掉客户端可能伪造的旧值
    if(!clientIp.empty()) {
        up.setHeader("X-Forwarded-For", clientIp);
        up.setHeader("X-Real-IP", clientIp);
    }
    if(!origHost.empty()) up.setHeader("X-Forwarded-Host", origHost);
    up.setHeader("X-Forwarded-Proto", proto);

    up.setHeader("Host", host_port(upstreamHost, upstreamPort));
    BodyFraming reqFraming = ctx.connection()->req_body_framing();
    if(reqFraming == BodyFraming::CHUNKED) {
        up.delHeader("Content-Length");
        up.setHeader("Transfer-Encoding", "chunked");
    } else if(reqFraming == BodyFraming::CONTENT_LENGTH) {
        up.delHeader("Transfer-Encoding");
        up.setHeader("Content-Length", std::to_string(ctx.connection()->req_clen_of()));
    } else {
        up.delHeader("Content-Length");
        up.delHeader("Transfer-Encoding");
    }
    up.setHeader("Connection", "keep-alive");
    return up.dumpHead();
}

ForwardResult Upstream::forward(ReqCtx& ctx,
                                uint64_t connectTimeoutMs, uint64_t recvTimeoutMs,
                                const std::vector<bronx::ipban::Ip>& trusted) {
    const auto& route = ctx.route();
    auto conn = ctx.connection();
    auto& metrics = GatewayMetrics::instance();

    bool hasReqBody = conn->req_body_framing() != BodyFraming::NONE;
    HeaderList reqHeaders = eff_headers(ctx.request());
    std::string expect = join_hdr_vals(reqHeaders, "Expect");
    if(!expect.empty() && !only_token(expect, "100-continue")) {
        return ForwardResult::EXPECT_FAIL;
    }
    bool sendContinue = hasReqBody && !expect.empty();
    bool retry = !hasReqBody && can_retry(ctx.request()->getMethod());
    auto group = route.upstream;
    if(!group) {
        BRONX_LOG_WARN(g_logger) << "no upstream group for route: " << route.routeKey;
        return ForwardResult::CONNECT_FAIL;
    }

    uint64_t totalMs = group->totalMs() ? group->totalMs() : recvTimeoutMs;
    uint64_t connectMs = group->connectMs() ? group->connectMs() : connectTimeoutMs;
    uint64_t readMs = group->readMs() ? group->readMs() : recvTimeoutMs;
    uint64_t begin = upMs();
    uint64_t dueMs = totalMs ? begin + totalMs : 0;
    if(dueMs && dueMs < begin) dueMs = UINT64_MAX;

    auto cap = [&](uint64_t want) {
        if(!dueMs) return want;
        return std::min(want, leftMs(dueMs));
    };
    auto count = [&](const UpRet& ret) {
        metrics.recordUp(ret);
    };

    AcqConn acquired;
    std::string upHost;
    uint16_t    upPort = 80;

    acquired = group->tryAcquire(connectMs, dueMs);
    if(!acquired.endpoint) {
        metrics.incr_acq_fail();
        UpRet ret;
        ret.why = acqWhy(acquired.why);
        ret.costMs = upMs() - begin;
        switch(acquired.why) {
            case AcqWhy::OPEN:
                metrics.incrCircuitOpen();
                ret.mark = UpMark::SKIP;
                return ForwardResult::UPSTREAM_BUSY;
            case AcqWhy::DOWN:
                metrics.incr_no_healthy();
                ret.mark = UpMark::SKIP;
                return ForwardResult::UPSTREAM_BUSY;
            case AcqWhy::BUSY:
                ret.mark = UpMark::SKIP;
                return ForwardResult::UPSTREAM_BUSY;
            case AcqWhy::DEADLINE:
                ret.mark = UpMark::SKIP;
                return ForwardResult::UPSTREAM_TIMEOUT;
            case AcqWhy::TIMEOUT:
                ret.mark = UpMark::FAIL;
                count(ret);
                return ForwardResult::UPSTREAM_TIMEOUT;
            case AcqWhy::CONNECT:
            case AcqWhy::NONE:
                ret.mark = UpMark::FAIL;
                ret.why = UpWhy::CONNECT;
                count(ret);
                return ForwardResult::CONNECT_FAIL;
        }
    }
    upHost = acquired.endpoint->host;
    upPort = acquired.endpoint->port;

    auto& usock = acquired.sock;
    auto arm = [&](uint64_t want) {
        uint64_t wait = cap(want);
        if(wait == 0) return false;
        usock->setRecvTimeout(wait);
        usock->setSendTimeout(wait);
        return true;
    };
    auto end = [&](ForwardResult out, UpRet ret, bool reusable = false) {
        if(ret.costMs == 0) ret.costMs = upMs() - begin;
        group->release(acquired, ret, reusable);
        count(ret);
        return out;
    };

    if(!arm(readMs)) {
        return end(ForwardResult::UPSTREAM_TIMEOUT,
                   UpRet{UpMark::FAIL, UpWhy::TIMEOUT});
    }

    std::string head = build_req_head(ctx, upHost, upPort, trusted);
    if(SendAll(usock, head.data(), head.size()) <= 0) {
        if(acquired.reused && retry) {
            uint64_t wait = cap(connectMs);
            auto fresh = wait ? acquired.endpoint->pool->createFresh(wait) : nullptr;
            if(fresh) {
                acquired.sock = fresh;
                acquired.reused = false;
                usock = acquired.sock;
                if(!arm(readMs)) {
                    return end(ForwardResult::UPSTREAM_TIMEOUT,
                               UpRet{UpMark::FAIL, UpWhy::TIMEOUT});
                }
                if(SendAll(usock, head.data(), head.size()) <= 0) {
                    bool timeout = errno == ETIMEDOUT || (dueMs && leftMs(dueMs) == 0);
                    return end(timeout ? ForwardResult::UPSTREAM_TIMEOUT : ForwardResult::SEND_FAIL,
                               UpRet{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::SEND});
                }
            } else {
                bool timeout = wait == 0 || errno == ETIMEDOUT;
                return end(timeout ? ForwardResult::UPSTREAM_TIMEOUT : ForwardResult::CONNECT_FAIL,
                           UpRet{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::CONNECT});
            }
        } else {
            bool timeout = errno == ETIMEDOUT || (dueMs && leftMs(dueMs) == 0);
            return end(timeout ? ForwardResult::UPSTREAM_TIMEOUT : ForwardResult::SEND_FAIL,
                       UpRet{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::SEND});
        }
    }

    if(sendContinue) {
        static const char msg[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if(SendAll(conn->socket(), msg, sizeof(msg) - 1) <= 0) {
            writeFail(ctx);
            return end(ForwardResult::OK, UpRet{UpMark::SKIP, UpWhy::BAD_BODY});
        }
    }

    // 流式推请求体
    auto reqBody = ctx.requestBody();
    BodyFraming reqFraming = conn->req_body_framing();
    BodyFraming upstreamReqFraming =
        reqFraming == BodyFraming::CHUNKED ? BodyFraming::CHUNKED : BodyFraming::CONTENT_LENGTH;
    if(reqBody) {
        std::string chunk;
        int n;
        while((n = reqBody->readChunk(chunk)) > 0) {
            if(!arm(readMs)) {
                return end(ForwardResult::UPSTREAM_TIMEOUT,
                           UpRet{UpMark::FAIL, UpWhy::TIMEOUT});
            }
            std::string out = BodyEncoder::encodeChunk(upstreamReqFraming, chunk.data(), chunk.size());
            if(SendAll(usock, out.data(), out.size()) <= 0) {
                bool timeout = errno == ETIMEDOUT || (dueMs && leftMs(dueMs) == 0);
                return end(timeout ? ForwardResult::UPSTREAM_TIMEOUT : ForwardResult::SEND_FAIL,
                           UpRet{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::SEND});
            }
            chunk.clear();
        }
        if(reqBody->hasError()) {
            ForwardResult out = reqBody->errorCode() == BodyReadError::TOO_LARGE
                ? ForwardResult::REQUEST_TOO_LARGE : ForwardResult::BAD_REQUEST_BODY;
            return end(out, UpRet{UpMark::SKIP, UpWhy::BAD_BODY});
        }
        std::string tail = BodyEncoder::encodeEnd(upstreamReqFraming);
        if(!tail.empty() && SendAll(usock, tail.data(), tail.size()) <= 0) {
            bool timeout = errno == ETIMEDOUT || (dueMs && leftMs(dueMs) == 0);
            return end(timeout ? ForwardResult::UPSTREAM_TIMEOUT : ForwardResult::SEND_FAIL,
                       UpRet{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::SEND});
        }
    }

    InBuf ubuf(4096);
    size_t headLen = 0;
    bool retriedRead = false;
    static constexpr size_t kMaxHead = 64 * 1024;
    while((headLen = ubuf.findHeaderEnd()) == 0) {
        if(ubuf.readable() >= kMaxHead) {
            return end(ForwardResult::BAD_RESPONSE,
                       UpRet{UpMark::FAIL, UpWhy::BAD_RESP});
        }
        if(!arm(readMs)) {
            return end(ForwardResult::UPSTREAM_TIMEOUT,
                       UpRet{UpMark::FAIL, UpWhy::TIMEOUT});
        }
        int rt = ubuf.fill(usock.get());
        if(rt == 0) {
            if(acquired.reused && retry && !retriedRead && ubuf.readable() == 0) {
                uint64_t wait = cap(connectMs);
                auto fresh = wait ? acquired.endpoint->pool->createFresh(wait) : nullptr;
                if(fresh) {
                    acquired.sock = fresh;
                    acquired.reused = false;
                    retriedRead = true;
                    usock = acquired.sock;
                    if(!arm(readMs)) {
                        return end(ForwardResult::UPSTREAM_TIMEOUT,
                                   UpRet{UpMark::FAIL, UpWhy::TIMEOUT});
                    }
                    if(SendAll(usock, head.data(), head.size()) <= 0) {
                        bool timeout = errno == ETIMEDOUT || (dueMs && leftMs(dueMs) == 0);
                        return end(timeout ? ForwardResult::UPSTREAM_TIMEOUT : ForwardResult::SEND_FAIL,
                                   UpRet{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::SEND});
                    }
                    continue;
                }
            }
            return end(ForwardResult::BAD_RESPONSE,
                       UpRet{UpMark::FAIL, UpWhy::BAD_RESP});
        }
        if(rt < 0) {
            bool timeout = errno == ETIMEDOUT || (dueMs && leftMs(dueMs) == 0);
            return end(timeout ? ForwardResult::UPSTREAM_TIMEOUT : ForwardResult::BAD_RESPONSE,
                       UpRet{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::BAD_RESP});
        }
    }
    if(headLen > kMaxHead) {
        return end(ForwardResult::BAD_RESPONSE,
                   UpRet{UpMark::FAIL, UpWhy::BAD_RESP});
    }
    HttpRespParser::ptr rparser;
    GwResponse::ptr rsp;
    int infos = 0;
    while(true) {
        if(!arm(readMs)) {
            return end(ForwardResult::UPSTREAM_TIMEOUT,
                       UpRet{UpMark::FAIL, UpWhy::TIMEOUT});
        }
        ForwardResult rr = read_up_head(ubuf, usock, rparser, rsp, dueMs, readMs);
        if(rr != ForwardResult::OK) {
            bool timeout = rr == ForwardResult::UPSTREAM_TIMEOUT
                || (dueMs && leftMs(dueMs) == 0);
            return end(timeout ? ForwardResult::UPSTREAM_TIMEOUT : rr,
                       UpRet{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::BAD_RESP});
        }
        if(rsp->getStatus() == 101) {
            return end(ForwardResult::BAD_RESPONSE,
                       UpRet{UpMark::FAIL, UpWhy::BAD_RESP});
        }
        if(rsp->getStatus() < 100 || rsp->getStatus() >= 200) {
            break;
        }
        if(++infos > 8) {
            return end(ForwardResult::BAD_RESPONSE,
                       UpRet{UpMark::FAIL, UpWhy::BAD_RESP});
        }
    }

    UpRet headRet;
    headRet.mark = acquired.endpoint->cb->badStatus(rsp->getStatus())
        ? UpMark::FAIL : UpMark::OK;
    headRet.why = headRet.mark == UpMark::FAIL ? UpWhy::STATUS : UpWhy::NONE;
    headRet.status = rsp->getStatus();
    headRet.costMs = upMs() - begin;
    usock->setRecvTimeout(readMs);
    usock->setSendTimeout(readMs);

    BodyFraming upFraming = rparser->getBodyFraming();
    uint64_t upCL = rparser->getContentLength();
    if(resp_no_body(ctx.request()->getMethod(), rsp->getStatus())) {
        upFraming = BodyFraming::NONE; upCL = 0;
    }
    std::string contentType = trim_ascii(rsp->getHeader("Content-Type"));
    bool sse = contentType.size() >= 17
        && strncasecmp(contentType.c_str(), "text/event-stream", 17) == 0;
    bool counted = headRet.mark == UpMark::FAIL || upFraming == BodyFraming::NONE || sse;
    if(counted) {
        group->mark(acquired, headRet);
        count(headRet);
    }
    auto finish = [&](UpRet ret, bool reusable) {
        group->release(acquired, ret, reusable);
        if(!counted) {
            count(ret);
            counted = true;
        }
    };

    auto outRsp = std::make_shared<GwResponse>();
    outRsp->setStatus(rsp->getStatus());
    outRsp->setReason(rsp->getReason());
    HeaderList rspHeaders = eff_headers(rsp);
    std::string rspConnectionHeader = join_hdr_vals(rspHeaders, "Connection");
    for(const auto& h : rspHeaders) {
        // 剥逐跳头, 也剥 XFF 类(上游不该往响应里写这些, 防泄漏)
        if(is_hop_hdr(h.first) || hdr_has_token(rspConnectionHeader, h.first)
           || is_fwd_hdr(h.first)) continue;
        outRsp->addHeader(h.first, h.second);
    }
    applyHeaders(ctx, outRsp);
    clean_resp_hdrs(outRsp);
    if(upFraming == BodyFraming::CHUNKED || upFraming == BodyFraming::UNTIL_CLOSE) {
        outRsp->delHeader("Content-Length");
        outRsp->setHeader("Transfer-Encoding", "chunked");
    } else if(upFraming == BodyFraming::CONTENT_LENGTH) {
        outRsp->delHeader("Transfer-Encoding");
        outRsp->setHeader("Content-Length", std::to_string(upCL));
    } else if(upFraming == BodyFraming::NONE) {
        outRsp->delHeader("Transfer-Encoding");
        if(ctx.request()->getMethod() == HttpMethod::HEAD) {
            // HEAD 不发送 body,但应保留上游对对应 GET 响应声明的 Content-Length。
            if(rsp->hasHeader("Content-Length")) {
                outRsp->setHeader("Content-Length", rsp->getHeader("Content-Length"));
            } else {
                outRsp->delHeader("Content-Length");
            }
        } else if(resp_no_body(ctx.request()->getMethod(), rsp->getStatus())) {
            outRsp->delHeader("Content-Length");
        } else {
            outRsp->setHeader("Content-Length", "0");
        }
    }
    ctx.setResponse(outRsp);   // 回填 ctx，供访问日志/metrics 读取上游响应状态
    if(conn->sendResponseHeadRaw(outRsp) <= 0) {
        writeFail(ctx);
        finish(headRet, false);
        return ForwardResult::OK;
    }
    ctx.setHandled(true);
    ctx.commitResp(RespState::STREAM, outRsp->getStatus(), bronx::GetCurrentMs());

    // 流式回传响应体
    BodyFraming clientFraming =
        (upFraming == BodyFraming::CHUNKED || upFraming == BodyFraming::UNTIL_CLOSE)
        ? BodyFraming::CHUNKED : BodyFraming::CONTENT_LENGTH;
    BodyReader reader(upFraming, upCL, &ubuf, usock.get());
    std::string chunk;
    int n;
    if(!resp_no_body(ctx.request()->getMethod(), rsp->getStatus())) {
        while((n = reader.readChunk(chunk)) > 0) {
            std::string enc = BodyEncoder::encodeChunk(clientFraming, chunk.data(), chunk.size());
            if(conn->sendBodyChunk(enc.data(), enc.size()) <= 0) {
                writeFail(ctx);
                finish(headRet, false);
                return ForwardResult::OK;
            }
            chunk.clear();
        }
    } else {
        while((n = reader.readChunk(chunk)) > 0) {
            chunk.clear();
        }
    }

    UpRet bodyRet = headRet;
    if(reader.hasError()) {
        writeFail(ctx);
        if(ctx.request()) ctx.request()->setClose(true);
        if(!counted) {
            bodyRet.mark = UpMark::FAIL;
            bodyRet.why = UpWhy::BAD_BODY;
        }
    } else {
        std::string end = BodyEncoder::encodeEnd(clientFraming);
        if(!end.empty() && conn->sendBodyChunk(end.data(), end.size()) <= 0) {
            writeFail(ctx);
        }
    }

    // 连接可复用判定（必须全部满足，否则丢弃避免串包）：
    //   1. body 读取无错误
    //   2. 上游未要求关闭（Connection: close）
    //   3. 响应精确定界（CL/chunked/none，非 UNTIL_CLOSE——后者靠关连接定界，不可复用）
    //   4. body 读完后缓冲区已排空（无多余字节，否则下个响应会粘包/串包）
    bool reusable = !reader.hasError()
                 && !rsp->isClose()
                 && upFraming != BodyFraming::UNTIL_CLOSE
                 && ubuf.readable() == 0;
    finish(bodyRet, reusable);
    return ForwardResult::OK;
}

} // namespace gateway
} // namespace bronx
