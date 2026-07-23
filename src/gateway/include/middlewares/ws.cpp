#include "middlewares/stubs.h"
#include "str.h"
#include "mw.h"
#include "conn.h"
#include "http_msg.h"
#include "ups_group.h"
#include "router.h"
#include "http_parse.h"
#include "log.h"
#include "reactor.h"
#include "io_hook.h"
#include "fd_context.h"
#include "io.h"
#include "hash.h"
#include "metrics.h"
#include "util.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <strings.h>
#include <unistd.h>

namespace bronx {
namespace gateway {

// WebSocket 透明隧道。握手照走网关那套，路由鉴权限流选上游熔断都过一遍，
// 101 之后就不解析 WS 帧了，纯 TCP 字节泵对拷，所以 text binary ping close 什么帧都能过。
// 握手没请求体，头解析完就能转。101 后脱离 keep-alive 循环，关两端 socket 唤醒双泵退出。
// 这层不做帧级限速和消息大小控制，要生产级策略得另接帧解析层。

static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

static bool hdr_eq_token(const std::string& v, const char* token) {
    return strcasecmp(v.c_str(), token) == 0;
}

static std::string ws_accept_key(const std::string& key) {
    static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    return bronx::base64encode(bronx::sha1sum(key + kGuid));
}

static bool is_hop_hdr_keepup(const std::string& key) {
    static const char* hop[] = {
        "Keep-Alive", "Proxy-Authenticate", "Proxy-Authorization", "Proxy-Connection",
        "TE", "Trailer", "Trailers", "Transfer-Encoding"
    };
    for(auto h : hop) {
        if(strcasecmp(key.c_str(), h) == 0) return true;
    }
    return false;
}

static std::string rewrite_path(ReqCtx& ctx) {
    auto req = ctx.request();
    const auto& route = ctx.route();
    const RouteRule* rule = route.rule.get();

    std::string path = req ? req->getPath() : "/";
    if(rule && rule->stripPrefix && !rule->pathPattern.empty()) {
        if(path.compare(0, rule->pathPattern.size(), rule->pathPattern) == 0) {
            path = path.substr(rule->pathPattern.size());
            if(path.empty() || path[0] != '/') path = "/" + path;
        }
    }
    if(rule && !rule->rewritePrefix.empty()) {
        if(rule->rewritePrefix == "/") {
            path = path.empty() || path[0] != '/' ? "/" + path : path;
        } else if(!path.empty()
                  && rule->rewritePrefix.back() == '/' && path[0] == '/') {
            path = rule->rewritePrefix + path.substr(1);
        } else if(!path.empty()
                  && rule->rewritePrefix.back() != '/' && path[0] != '/') {
            path = rule->rewritePrefix + "/" + path;
        } else {
            path = rule->rewritePrefix + path;
        }
    }
    return path.empty() ? "/" : path;
}

static std::string build_upgrade_req(ReqCtx& ctx,
                                       const std::string& upstreamHost,
                                       uint32_t upstreamPort) {
    auto req = ctx.request();
    const RouteRule* rule = ctx.route().rule.get();

    GwRequest up;
    up.setMethod(req->getMethod());
    up.setMethodRaw(req->getMethodRaw().empty()
                    ? HttpMethodToString(req->getMethod()) : req->getMethodRaw());
    up.setVersion(0x11);
    up.setPath(rewrite_path(ctx));
    up.setQuery(req->getQuery());

    HeaderList headers = req->getHeaderList();
    if(headers.empty()) {
        for(const auto& h : req->getHeaders()) {
            headers.emplace_back(h.first, h.second);
        }
    }
    std::string connectionHeader = join_hdr_vals(headers, "Connection");
    for(const auto& h : headers) {
        if(is_hop_hdr_keepup(h.first)
                || (strcasecmp(h.first.c_str(), "Upgrade") != 0
                    && hdr_has_token(connectionHeader, h.first))) {
            continue;
        }
        up.addHeader(h.first, h.second);
    }
    if(rule) {
        std::string setConnection;
        for(const auto& kv : rule->reqHeaderSet) {
            if(strcasecmp(kv.first.c_str(), "Connection") == 0) {
                if(!setConnection.empty()) setConnection += ",";
                setConnection += kv.second;
            }
        }
        for(const auto& kv : rule->reqHeaderSet) {
            if(is_hop_hdr_keepup(kv.first) || hdr_has_token(setConnection, kv.first)
               || strcasecmp(kv.first.c_str(), "Connection") == 0
               || strcasecmp(kv.first.c_str(), "Upgrade") == 0) continue;
            up.setHeader(kv.first, kv.second);
        }
        for(const auto& k : rule->reqHeaderRemove) {
            up.delHeader(k);
        }
    }

    up.setHeader("Host", host_port(upstreamHost, upstreamPort));
    up.setHeader("Connection", "Upgrade");
    up.setHeader("Upgrade", "websocket");
    up.delHeader("Content-Length");
    up.delHeader("Transfer-Encoding");
    return up.dumpHead();
}

static bool read_resp_head(const bronx::BxSocket::ptr& sock,
                           uint64_t recvTimeoutMs,
                           uint64_t dueMs,
                           std::string& head,
                           std::string& extra) {
    // 走 hook recv:无数据时 armEvent(EV_IN)+yield,上游握手响应一到 epoll 精确唤醒,
    // 超时交给 SO_RCVTIMEO(setRecvTimeout)——不再 recv_f 裸探测 + usleep 主动轮询。
    sock->setRecvTimeout(recvTimeoutMs);
    std::string buf;
    buf.reserve(4096);
    size_t headLen = 0;
    char tmp[4096];
    while((headLen = buf.find("\r\n\r\n")) == std::string::npos) {
        if(buf.size() > 64 * 1024) return false;
        if(dueMs) {
            uint64_t wait = std::min(recvTimeoutMs, leftMs(dueMs));
            if(wait == 0) {
                errno = ETIMEDOUT;
                return false;
            }
            sock->setRecvTimeout(wait);
        }
        int rt = sock->recv(tmp, sizeof(tmp));
        if(rt > 0) {
            buf.append(tmp, (size_t)rt);
            continue;
        }
        // rt==0 对端关闭;rt<0 出错/超时(errno 已由 hook 置 ETIMEDOUT/ECANCELED 等)
        return false;
    }
    headLen += 4;
    head.assign(buf.data(), headLen);
    if(buf.size() > headLen) {
        extra.assign(buf.data() + headLen, buf.size() - headLen);
    }
    return true;
}

static GwResponse::ptr parse_101(const std::string& head,
                                 const std::string& clientKey) {
    auto parser = std::make_shared<HttpRespParser>();
    parser->execute(head.c_str(), head.size());
    if(parser->hasError() || !parser->isFinished()) {
        return nullptr;
    }
    auto rsp = parser->getData();
    if(rsp->getStatus() != 101) {
        return nullptr;
    }
    std::string key = trim_ascii(clientKey);
    bool ok = hdr_eq_token(trim_ascii(rsp->getHeader("Upgrade")), "websocket")
        && hdr_has_token(rsp->getHeader("Connection"), "Upgrade")
        && !key.empty()
        && trim_ascii(rsp->getHeader("Sec-WebSocket-Accept")) == ws_accept_key(key);
    return ok ? rsp : nullptr;
}

static GwResponse::ptr client_101(ReqCtx& ctx, const GwResponse::ptr& upstream) {
    auto rsp = std::make_shared<GwResponse>();
    rsp->setVersion(0x11);
    rsp->setStatus(101);
    rsp->setReason(upstream->getReason());
    rsp->setHeader("Upgrade", "websocket");
    rsp->setHeader("Connection", "Upgrade");
    for(const char* name : {"Sec-WebSocket-Accept", "Sec-WebSocket-Protocol",
                            "Sec-WebSocket-Extensions"}) {
        std::string value = upstream->getHeader(name);
        if(!value.empty()) rsp->setHeader(name, value);
    }
    applyHeaders(ctx, rsp);
    return rsp;
}

struct TunnelState {
    std::atomic<bool> closed{false};
    std::atomic<bool> released{false};
    std::atomic<bool> writeFailed{false};
    std::atomic<bool> idle{false};
    std::atomic<int> activePumps{2};
    std::atomic<int> end{0};
    int doneFd = -1;
    UpstreamGroup::ptr group;
    Endpoint* endpoint = nullptr;
    bronx::BxSocket::ptr client;
    bronx::BxSocket::ptr upstream;
    uint64_t idleTimeoutMs = 60000;
};

class PumpWait {
public:
    PumpWait()
        : m_fd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
        if(m_fd < 0) return;
        auto ctx = bronx::FdMgr::GetInstance()->get(m_fd, true);
        if(!ctx || ctx->isClose()) {
            ::close(m_fd);
            m_fd = -1;
            return;
        }
        ctx->setHookNonblock(true);
        ctx->setSysNonblock(true);
        ctx->setUserNonblock(false);
    }

    ~PumpWait() {
        if(m_fd >= 0) ::close(m_fd);
    }

    int fd() const { return m_fd; }

    bool wait() const {
        uint64_t n = 0;
        while(true) {
            ssize_t rt = ::read(m_fd, &n, sizeof(n));
            if(rt == (ssize_t)sizeof(n)) return true;
            if(rt < 0 && errno == EINTR) continue;
            return false;
        }
    }

private:
    int m_fd = -1;
};

static void wake_pump(int fd) {
    if(fd < 0) return;
    uint64_t one = 1;
    while(true) {
        ssize_t n = write_f(fd, &one, sizeof(one));
        if(n == (ssize_t)sizeof(one)) return;
        if(n < 0 && errno == EINTR) continue;
        return;
    }
}

static void set_end(const std::shared_ptr<TunnelState>& st, int end) {
    int open = 0;
    st->end.compare_exchange_strong(open, end);
}

static const char* end_name(int end) {
    switch(end) {
        case 1: return "peer";
        case 2: return "idle";
        case 3: return "io";
        case 4: return "write";
        default: return "closed";
    }
}

static void close_tunnel(const std::shared_ptr<TunnelState>& st) {
    bool expected = false;
    if(st->closed.compare_exchange_strong(expected, true)) {
        if(st->client && st->client->getSocket() >= 0) {
            ::shutdown(st->client->getSocket(), SHUT_RDWR);
        }
        if(st->upstream && st->upstream->getSocket() >= 0) {
            ::shutdown(st->upstream->getSocket(), SHUT_RDWR);
        }
    }
}

static void half_close_wr(const bronx::BxSocket::ptr& sock) {
    if(sock && sock->getSocket() >= 0) {
        ::shutdown(sock->getSocket(), SHUT_WR);
    }
}

static void release_tunnel(const std::shared_ptr<TunnelState>& st) {
    bool expected = false;
    if(st->released.compare_exchange_strong(expected, true)) {
        if(st->endpoint) st->endpoint->dropHold();
    }
}

static void finish_pump(const std::shared_ptr<TunnelState>& st, bool fatal) {
    if(fatal) {
        close_tunnel(st);
    }
    int left = st->activePumps.fetch_sub(1) - 1;
    if(left == 0) {
        release_tunnel(st);
        if(st->client) st->client->close();
        if(st->upstream) st->upstream->close();
        wake_pump(st->doneFd);
    }
}

static void pump_tunnel(const bronx::BxSocket::ptr& from,
                       const bronx::BxSocket::ptr& to,
                       const std::shared_ptr<TunnelState>& st) {
    char buf[16 * 1024];
    bool fatal = false;
    auto idleTimeout = std::chrono::milliseconds(st->idleTimeoutMs);
    auto idleDeadline = std::chrono::steady_clock::now() + idleTimeout;
    while(!st->closed.load(std::memory_order_relaxed)) {
        errno = 0;
        int n = from->recv(buf, sizeof(buf));
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)) {
            if(std::chrono::steady_clock::now() >= idleDeadline) {
                st->idle.store(true);
                set_end(st, 2);
                break;
            }
            usleep(1000);
            continue;
        }
        if(n <= 0) {
            if(n < 0 && errno == ETIMEDOUT) st->idle.store(true);
            set_end(st, n == 0 ? 1 : (errno == ETIMEDOUT ? 2 : 3));
            if(n < 0 && errno != ECANCELED && errno != EBADF) {
                BRONX_LOG_WARN(g_logger) << "ws tunnel pump end n=" << n
                                         << " errno=" << errno
                                         << " errstr=" << strerror(errno);
                fatal = errno != ETIMEDOUT;
            }
            break;
        }
        idleDeadline = std::chrono::steady_clock::now() + idleTimeout;
        if(SendAll(to, buf, (size_t)n) <= 0) {
            st->writeFailed.store(true);
            set_end(st, 4);
            BRONX_LOG_WARN(g_logger) << "ws tunnel pump send failed errno="
                                     << errno << " errstr=" << strerror(errno);
            fatal = errno != EPIPE && errno != ECONNRESET && errno != ECANCELED;
            break;
        }
    }
    if(!fatal) {
        half_close_wr(to);
    }
    finish_pump(st, fatal);
}

static void tunnel(ReqCtx& ctx, uint64_t connectTimeoutMs, uint64_t recvTimeoutMs) {
    if(!ctx.connection()) {
        reply(ctx, 503);
        return;
    }
    auto client = ctx.connection()->socket();
    auto group = ctx.route().upstream;
    if(!group) {
        BRONX_LOG_WARN(g_logger) << "ws tunnel no upstream group";
        reply(ctx, 502);
        return;
    }

    uint64_t begin = upMs();
    uint64_t totalMs = group->totalMs() ? group->totalMs() : recvTimeoutMs;
    uint64_t connectMs = group->connectMs() ? group->connectMs() : connectTimeoutMs;
    uint64_t headMs = group->readMs() ? group->readMs() : recvTimeoutMs;
    uint64_t dueMs = totalMs ? begin + totalMs : 0;
    if(dueMs && dueMs < begin) dueMs = UINT64_MAX;
    auto cap = [&](uint64_t want) {
        return dueMs ? std::min(want, leftMs(dueMs)) : want;
    };

    auto acquired = group->tryAcquire(connectMs, dueMs);
    if(!acquired.endpoint || !acquired.sock) {
        auto& metrics = GatewayMetrics::instance();
        metrics.incr_acq_fail();
        UpRet ret;
        ret.why = acqWhy(acquired.why);
        ret.costMs = upMs() - begin;
        switch(acquired.why) {
            case AcqWhy::OPEN:
                metrics.incrCircuitOpen();
                ret.mark = UpMark::SKIP;
                break;
            case AcqWhy::DOWN:
                metrics.incr_no_healthy();
                ret.mark = UpMark::SKIP;
                break;
            case AcqWhy::BUSY:
            case AcqWhy::DEADLINE:
                ret.mark = UpMark::SKIP;
                break;
            case AcqWhy::TIMEOUT:
                ret.mark = UpMark::FAIL;
                break;
            case AcqWhy::CONNECT:
            case AcqWhy::NONE:
                ret.mark = UpMark::FAIL;
                ret.why = UpWhy::CONNECT;
                break;
        }
        metrics.recordUp(ret);
        BRONX_LOG_WARN(g_logger) << "ws tunnel acquire upstream failed";
        int status = 502;
        if(acquired.why == AcqWhy::BUSY || acquired.why == AcqWhy::OPEN
                || acquired.why == AcqWhy::DOWN) status = 503;
        if(acquired.why == AcqWhy::TIMEOUT || acquired.why == AcqWhy::DEADLINE) status = 504;
        reply(ctx, status);
        return;
    }
    uint64_t wait = cap(headMs);
    if(wait == 0) {
        group->release(acquired, UpRet{UpMark::FAIL, UpWhy::TIMEOUT});
        GatewayMetrics::instance().recordUp(UpRet{UpMark::FAIL, UpWhy::TIMEOUT});
        reply(ctx, 504);
        return;
    }
    acquired.sock->setRecvTimeout((int64_t)wait);
    acquired.sock->setSendTimeout((int64_t)wait);

    auto release = [&](UpRet ret) {
        if(group && acquired.endpoint) {
            if(ret.costMs == 0) ret.costMs = upMs() - begin;
            group->release(acquired, ret, false);
            GatewayMetrics::instance().recordUp(ret);
        }
    };

    std::string reqHead = build_upgrade_req(ctx, acquired.endpoint->host, acquired.endpoint->port);
    if(SendAll(acquired.sock, reqHead.data(), reqHead.size()) <= 0) {
        BRONX_LOG_WARN(g_logger) << "ws tunnel send upstream handshake failed endpoint="
                                 << acquired.endpoint->host << ":" << acquired.endpoint->port
                                 << " errno=" << errno << " errstr=" << strerror(errno);
        bool timeout = errno == ETIMEDOUT || (dueMs && leftMs(dueMs) == 0);
        release(UpRet{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::SEND});
        reply(ctx, timeout ? 504 : 502);
        return;
    }

    std::string rspHead;
    std::string extra;
    wait = cap(headMs);
    if(wait == 0 || !read_resp_head(acquired.sock, wait, dueMs, rspHead, extra)) {
        BRONX_LOG_WARN(g_logger) << "ws tunnel read upstream handshake failed endpoint="
                                 << acquired.endpoint->host << ":" << acquired.endpoint->port
                                 << " errno=" << errno << " errstr=" << strerror(errno);
        bool timeout = wait == 0 || errno == ETIMEDOUT || (dueMs && leftMs(dueMs) == 0);
        release(UpRet{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::BAD_RESP});
        reply(ctx, timeout ? 504 : 502);
        return;
    }
    auto upstreamRsp = parse_101(rspHead, ctx.request()->getHeader("Sec-WebSocket-Key"));
    if(!upstreamRsp) {
        BRONX_LOG_WARN(g_logger) << "ws tunnel bad upstream handshake head="
                                 << rspHead.substr(0, 120);
        release(UpRet{UpMark::FAIL, UpWhy::BAD_RESP});
        reply(ctx, 502);
        return;
    }

    UpRet headRet{UpMark::OK, UpWhy::NONE, 101, upMs() - begin};
    group->mark(acquired, headRet);
    GatewayMetrics::instance().recordUp(headRet);

    auto rsp = client_101(ctx, upstreamRsp);
    ctx.setResponse(rsp);
    std::string outHead = rsp->dumpHead();
    if(SendAll(client, outHead.data(), outHead.size()) <= 0) {
        writeFail(ctx);
        group->release(acquired, headRet, false);
        return;
    }

    ctx.setHandled(true);
    ctx.connection()->mark_raw_sent(101);
    ctx.commitResp(RespState::TUNNEL, 101, bronx::GetCurrentMs());
    auto& metrics = GatewayMetrics::instance();
    uint64_t wsStart = bronx::GetCurrentMs();
    metrics.wsOpen();
    auto finishWs = [&]() {
        uint64_t now = bronx::GetCurrentMs();
        uint64_t duration = now >= wsStart ? now - wsStart : 0;
        metrics.recordWsDuration(duration);
        metrics.wsClose();
        BRONX_LOG_INFO(g_logger) << "ws tunnel close reason=write duration_ms=" << duration;
    };

    if(!extra.empty() && SendAll(client, extra.data(), extra.size()) <= 0) {
        writeFail(ctx);
        group->release(acquired, headRet, false);
        finishWs();
        return;
    }

    // 头解析用 InBuf 可能一次 recv 多读到 101 后的首帧。
    // 升级成功后这些字节已经属于 WS 数据流,必须先交给上游再进入读泵。
    std::string clientBuffered = ctx.connection()->takeBufferedInput();
    if(!clientBuffered.empty() &&
            SendAll(acquired.sock, clientBuffered.data(), clientBuffered.size()) <= 0) {
        BRONX_LOG_WARN(g_logger) << "ws buffered client bytes send failed len="
                                 << clientBuffered.size();
        writeFail(ctx);
        group->release(acquired, headRet, false);
        finishWs();
        return;
    }

    auto st = std::make_shared<TunnelState>();
    st->group = group;
    st->endpoint = acquired.endpoint;
    st->client = client;
    st->upstream = acquired.sock;
    st->idleTimeoutMs = recvTimeoutMs;
    acquired.sock.reset();
    acquired.endpoint = nullptr;

    PumpWait done;
    if(done.fd() < 0) {
        set_end(st, 3);
        close_tunnel(st);
        release_tunnel(st);
        writeFail(ctx);
        finishWs();
        return;
    }
    st->doneFd = done.fd();

    st->client->setRecvTimeout((int64_t)recvTimeoutMs);
    st->client->setSendTimeout((int64_t)recvTimeoutMs);
    st->upstream->setRecvTimeout((int64_t)recvTimeoutMs);
    st->upstream->setSendTimeout((int64_t)recvTimeoutMs);

    bronx::BxIoManager* iom = bronx::BxIoManager::Current();
    if(iom) {
        iom->post([st]() {
            pump_tunnel(st->upstream, st->client, st);
        });
    } else {
        st->activePumps.store(1);
    }
    pump_tunnel(st->client, st->upstream, st);
    if(st->activePumps.load() > 0 && !done.wait()) {
        set_end(st, 3);
        close_tunnel(st);
    }
    if(st->writeFailed.load()) {
        writeFail(ctx);
    }
    uint64_t now = bronx::GetCurrentMs();
    uint64_t duration = now >= wsStart ? now - wsStart : 0;
    metrics.recordWsDuration(duration);
    metrics.wsClose();
    const char* why = st->writeFailed.load() ? "write"
                    : st->idle.load() ? "idle" : end_name(st->end.load());
    BRONX_LOG_INFO(g_logger) << "ws tunnel close reason=" << why
                             << " duration_ms=" << duration;
}

Middleware::ptr MakeWebSocketTunnelStub(uint64_t connectMs, uint64_t idleMs) {
    return std::make_shared<FuncMiddleware>(
        [connectMs, idleMs](ReqCtx& ctx, const NextFn& next) {
            auto req = ctx.request();
            if(!req || !req->isWebsocket()) {
                next();
                return;
            }
            tunnel(ctx, connectMs, idleMs);
        }, "ws_tunnel");
}

} // namespace gateway
} // namespace bronx
