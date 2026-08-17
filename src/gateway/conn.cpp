#include "conn.h"
#include "gateway.h"
#include "metrics.h"
#include "net_socket.h"
#include "log.h"
#include "io.h"
#include "endpoint.h"
#include "trace.h"
#include "util.h"
#include <chrono>
#include <netinet/in.h>

namespace bronx {
namespace gateway {

static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

namespace {
constexpr size_t kShrinkCapacityThreshold = 64 * 1024;
constexpr size_t kLowUsageThreshold       = 8 * 1024;
constexpr size_t kShrinkTargetCapacity    = 8 * 1024;
constexpr size_t kShrinkReserveWritable   = 4 * 1024;
constexpr size_t kRequiredLowUsageRounds  = 4;
}

static bool resp_no_body(const GwRequest::ptr& req, int status) {
    return (req && req->getMethod() == HttpMethod::HEAD)
        || (status >= 100 && status < 200)
        || status == 204
        || status == 304;
}

GatewayConnection::GatewayConnection(std::shared_ptr<bronx::BxSocket> sock, GatewayServer* server)
    : m_sock(std::move(sock))
    , m_server(server)
    , m_input(4096) {}

void GatewayConnection::startTrace(const std::string& clientIp) {
    if(!m_request || !TraceGate::instance().on()) return;
    if(!m_trace.id) m_trace.id = TraceTag::nextId();
    m_trace.on = TraceGate::instance().want(clientIp, m_request->getPath());
    // path 已知后才允许连接级打点，不能把旧 keep-alive 请求的命中状态带到下一条。
    m_trace.connOn = m_trace.on;
    GW_TRACE(m_trace, "req  head  "
        << HttpMethodToString(m_request->getMethod()) << " " << m_request->getPath()
        << (m_request->getQuery().empty() ? "" : "?") << m_request->getQuery()
        << " framing=" << framingName(m_reqFraming) << " clen=" << m_req_clen
        << " hdrs=" << m_request->getHeaders().size()
        << " ka=" << (m_request->isClose() ? 0 : 1));
}

// keep-alive 循环驱动
void GatewayConnection::process() {
    const auto& opts = m_server->options();
    auto& metrics = GatewayMetrics::instance();
    metrics.incrConnections();
    while(true) {
        // 优雅停机:正在 drain 则不再处理新请求
        if(m_server->isDraining()) {
            break;
        }
        // keep-alive 请求数上限
        if(m_req_count >= opts.maxKeepAliveRequests) {
            break;
        }

        m_sock->setRecvTimeout(m_req_count == 0 ? opts.headerTimeoutMs : opts.idleTimeoutMs);
        m_request.reset();
        m_bodyReader.reset();
        m_responded = false;
        m_writeFailed = false;
        m_lastStatus = 0;
        m_cut = ReqCut::NONE;
        reset_use_round();
        m_trace.reset(m_req_count + 1);
        auto reqStart = std::chrono::steady_clock::now();

        int rt = read_req_head();
        if(rt <= 0) {
            if(rt < 0 && (m_lastStatus != 0 || m_cut != ReqCut::NONE)) {
                uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - reqStart).count();
                metrics.finishReq(ReqStage::HEAD,
                    m_writeFailed ? ReqResult::WRITE_FAIL : ReqResult::REJECT,
                    m_lastStatus, m_cut, us);
            }
            break;
        }
        ++m_req_count;

        // 交给 handler
        const auto& handler = m_server->requestHandler();
        if(handler) {
            handler(*this);
        } else {
            auto rsp = std::make_shared<GwResponse>();
            rsp->setStatus(200);
            sendResponse(rsp, "");
        }

        bool stop = false;
        if(m_bodyReader && m_bodyReader->hasError() && !m_responded) {
            bool large = m_bodyReader->errorCode() == BodyReadError::TOO_LARGE;
            m_cut = large ? ReqCut::BODY_TOO_LARGE : ReqCut::BAD_BODY;
            sendError(large ? 413 : 400);
            stop = true;
        }

        if(!m_responded) {
            if(!m_writeFailed) {
                m_cut = ReqCut::NO_RESPONSE;
                sendError(500);
            }
            stop = true;
        }

        uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - reqStart).count();
        ReqResult result = m_writeFailed ? ReqResult::WRITE_FAIL
            : (m_cut != ReqCut::NONE ? ReqResult::REJECT : ReqResult::OK);
        metrics.finishReq(ReqStage::HANDLE, result, m_lastStatus, m_cut, us);

        GW_TRACE(m_trace, "req  done  status=" << m_lastStatus
                          << " cost=" << (us / 1000.0) << "ms"
                          << " cut=" << reqCutName(m_cut)
                          << (m_writeFailed ? " write_fail" : "")
                          << (stop ? " stop" : ""));

        if(stop) {
            break;
        }

        // 先判要不要关: 请求要求关 / 不允许 keep-alive / 正在 drain。确定要关就别再
        // 排空 body 了(名单封禁等短路场景已标 close), 省得为马上要丢的连接白读整个
        // body(最大 16MB)。只有决定复用连接时才排空未读完的 body。
        if(!opts.keepAlive || m_request->isClose() || m_server->isDraining()) {
            break;
        }
        if(!drain_req_body()) {
            break;   // 排空失败(连接坏)→ 关
        }

        maybe_trim_buf();
    }
    GW_TRACE_C(m_trace, "conn close reqs=" << m_req_count);
    metrics.decrConnections();
    m_sock->close();
}

// <!-- PLACEHOLDER_CONN -->

int GatewayConnection::fillInput() {
    int n = m_input.fill(m_sock.get());
    if(n > 0 && m_input.readable() > m_peak_readable) {
        m_peak_readable = m_input.readable();
    }
    GW_TRACE2(m_trace, "cli  recv  n=" << n << " buf=" << m_input.readable()
                       << (n < 0 ? std::string(" errno=") + std::to_string(errno) : std::string()));
    return n;
}

void GatewayConnection::reset_use_round() {
    m_peak_readable = m_input.readable();
}

void GatewayConnection::maybe_trim_buf() {
    size_t cap = m_input.capacity();
    size_t cur = m_input.readable();
    if(cap < kShrinkCapacityThreshold) {
        m_low_use_rounds = 0;
        reset_use_round();
        return;
    }
    if(m_peak_readable > kLowUsageThreshold || cur > kLowUsageThreshold) {
        m_low_use_rounds = 0;
        reset_use_round();
        return;
    }

    ++m_low_use_rounds;
    if(m_low_use_rounds >= kRequiredLowUsageRounds) {
        // 连续低使用 + InBuf 自身滞回,避免大小请求交替时反复扩缩。
        m_input.trim(kShrinkTargetCapacity, kShrinkReserveWritable);
        m_low_use_rounds = 0;
    }
    reset_use_round();
}

// 读并解析一个请求头:累积字节到 \r\n\r\n(Mongrel 非增量,见 gateway-project)→ 单次解析。
// 返回 1 成功 / 0 对端关闭 / <0 出错。
int GatewayConnection::read_req_head() {
    const auto& opts = m_server->options();
    // 读 socket 直到出现完整头块
    size_t headLen = 0;
    while((headLen = m_input.findHeaderEnd()) == 0) {
        // 超过头块上限 → 431
        if(m_input.readable() > opts.maxHeaderSize) {
            m_cut = ReqCut::HEADER_TOO_LARGE;
            sendError(431);
            return -1;
        }
        int n = fillInput();
        if(n == 0) {
            if(m_input.readable() > 0) {
                m_cut = ReqCut::BAD_HEADER;
                sendError(400);
                return -1;
            }
            return 0;        // 对端正常关闭(可能是 keep-alive 等待中关闭)
        }
        if(n < 0) {
            if(m_input.readable() > 0) m_cut = ReqCut::BAD_HEADER;
            return -1;       // 错误/超时/取消
        }
    }
    // 头块到齐,单次解析(off=0)
    if(headLen > opts.maxHeaderSize) {
        m_cut = ReqCut::HEADER_TOO_LARGE;
        sendError(431);
        return -1;
    }
    auto parser = std::make_shared<HttpReqParser>();
    size_t nparse = parser->execute(m_input.peek(), headLen);
    if(parser->hasError() || !parser->isFinished()) {
        m_cut = ReqCut::BAD_HEADER;
        sendError(400);
        return -1;
    }
    m_request = parser->getData();
    // HTTP/1.0 默认关闭(除非显式 keep-alive,已在解析器按 Connection 头设置)
    if(m_request->getVersion() == 0x10 && !m_request->hasHeader("Connection")) {
        m_request->setClose(true);
    }
    // 确定 body 分帧(含走私判定)
    m_reqFraming = parser->getBodyFraming();
    if(parser->hasError()) {     // getBodyFraming 可能置走私错误
        m_cut = ReqCut::BAD_HEADER;
        sendError(400);
        return -1;
    }
    m_req_clen = parser->getContentLength();
    // body 上限检查(CL 模式可提前判断)
    if(m_reqFraming == BodyFraming::CONTENT_LENGTH && m_req_clen > opts.maxBodySize) {
        m_cut = ReqCut::BODY_TOO_LARGE;
        sendError(413);
        return -1;
    }
    // 消费头块,剩余字节(body 起始)留在 buffer
    m_input.consume(headLen);
    // 建 body 读取器(惰性:handler 要才读)
    m_bodyReader = std::make_shared<BodyReader>(
        m_reqFraming, m_req_clen, &m_input, m_sock.get());
    m_bodyReader->setMaxBodySize(opts.maxBodySize);
    m_bodyReader->setFillObserver([this](size_t readable) {
        if(readable > m_peak_readable) {
            m_peak_readable = readable;
        }
        GW_TRACE2(m_trace, "cli  body recv buf=" << readable);
    });
    m_sock->setRecvTimeout(opts.bodyTimeoutMs);
    return 1;
}

BodyReader::ptr GatewayConnection::requestBody() {
    return m_bodyReader;
}

// keep-alive 复用前排空当前请求 body(handler 没读完的部分)
bool GatewayConnection::drain_req_body() {
    if(!m_bodyReader || m_bodyReader->isFinished()) {
        return true;
    }
    std::string sink;
    int n;
    while((n = m_bodyReader->readChunk(sink)) > 0) {
        sink.clear();   // 丢弃
    }
    return !m_bodyReader->hasError();
}

// 回写完整响应(自动 Content-Length)
int GatewayConnection::sendResponse(const GwResponse::ptr& rsp, const std::string& body) {
    rsp->setVersion(m_request ? m_request->getVersion() : 0x11);
    std::string outBody = body;
    force_body_err(rsp, &outBody);
    // 是否关闭:跟随请求意图
    bool close = !m_server->options().keepAlive || (m_request && m_request->isClose());
    bool noBody = resp_no_body(m_request, rsp->getStatus());
    rsp->setClose(close);
    if(noBody && !(m_request && m_request->getMethod() == HttpMethod::HEAD)) {
        rsp->delHeader("Content-Length");
    } else {
        rsp->setHeader("Content-Length", std::to_string(outBody.size()));
    }
    rsp->setHeader("Connection", close ? "close" : "keep-alive");

    // 合并 head + body 一次发出:避免小响应分成两个 TCP 段(省一次 syscall,
    // 且客户端单次 recv 能拿到完整响应)。完整响应已在内存,合并无额外开销。
    std::string out = rsp->dumpHead();
    if(!noBody) {
        out += outBody;
    }
    int sent = sendAll(out.data(), out.size());
    m_lastStatus = rsp->getStatus();
    if (sent <= 0) {
      if (m_request)
        m_request->setClose(true);
      return -1;
    }
    m_responded = true;
    if (close && m_request)
      m_request->setClose(true);
    return sent;
}

int GatewayConnection::req_body_err_status() const {
    if(!m_bodyReader || !m_bodyReader->hasError()) {
        return 0;
    }
    return m_bodyReader->errorCode() == BodyReadError::TOO_LARGE ? 413 : 400;
}

bool GatewayConnection::force_body_err(const GwResponse::ptr& rsp, std::string* body) {
    int status = req_body_err_status();
    if(status == 0 || rsp->getStatus() >= 400) {
        return false;
    }
    rsp->setStatus(status);
    m_cut = status == 413 ? ReqCut::BODY_TOO_LARGE : ReqCut::BAD_BODY;
    rsp->setReason("");
    rsp->setHeader("Content-Type", "text/plain");
    if(body) {
        *body = std::string(HttpStatusReason(status)) + "\n";
    }
    if(m_request) {
        m_request->setClose(true);
    }
    return true;
}

int GatewayConnection::sendAll(const char* data, size_t len) {
    int n = SendAll(m_sock, data, len);
    if(len && n <= 0) m_writeFailed = true;
    return n;
}

int GatewayConnection::sendResponseHead(const GwResponse::ptr& rsp) {
    rsp->setVersion(m_request ? m_request->getVersion() : 0x11);
    force_body_err(rsp, nullptr);
    std::string head = rsp->dumpHead();
    m_lastStatus = rsp->getStatus();
    int n = sendAll(head.data(), head.size());
    if(n > 0) m_responded = true;
    return n;
}

// 上游转发:回写上游响应头。保留上游状态码/头部,仅设与客户端的版本 + Connection。
// 转发场景下本轮与客户端用 close(每请求新上游连,简单可靠),故标记请求关闭。
int GatewayConnection::sendResponseHeadRaw(const GwResponse::ptr& rsp) {
    rsp->setVersion(m_request ? m_request->getVersion() : 0x11);
    force_body_err(rsp, nullptr);
    bool close = !m_server->options().keepAlive || (m_request && m_request->isClose());
    rsp->setHeader("Connection", close ? "close" : "keep-alive");
    if(close && m_request) m_request->setClose(true);
    std::string head = rsp->dumpHead();
    m_lastStatus = rsp->getStatus();
    int n = sendAll(head.data(), head.size());
    if (n <= 0) {
      if (m_request)
        m_request->setClose(true);
      return n;
    }
    m_responded = true;
    return n;
}

int GatewayConnection::sendBodyChunk(const char* data, size_t len) {
    return sendAll(data, len);
}

void GatewayConnection::mark_raw_sent(int status) {
    m_responded = true;
    m_lastStatus = status;
    if(m_request) m_request->setClose(true);
}

void GatewayConnection::markWriteFail(int status) {
    m_writeFailed = true;
    if(status > 0) m_lastStatus = status;
    if(m_request) m_request->setClose(true);
}

std::string GatewayConnection::takeBufferedInput() {
    std::string out;
    if(m_input.readable() > 0) {
        out.assign(m_input.peek(), m_input.readable());
        m_input.consumeAll();
    }
    return out;
}

// 简单错误响应(并标记关闭连接)
void GatewayConnection::sendError(int status) {
    auto rsp = std::make_shared<GwResponse>();
    rsp->setStatus(status);
    std::string body = std::string(HttpStatusReason(status)) + "\n";
    rsp->setHeader("Content-Type", "text/plain");
    if(m_request) m_request->setClose(true);   // 错误后关闭
    else {
        // 没解析出请求(如 400):构造最小响应
    }
    sendResponse(rsp, body);
}

std::string GatewayConnection::peerAddr() const {
    if(!m_sock) return "";
    auto addr = m_sock->getRemoteAddress();
    if(!addr) return "";
    if(auto ip = std::dynamic_pointer_cast<bronx::BxIpAddress>(addr)) {
        if(ip->getFamily() == AF_INET) {
            std::string s = addr->toString();
            auto colon = s.rfind(':');
            return colon == std::string::npos ? s : s.substr(0, colon);
        }
        if(ip->getFamily() == AF_INET6) {
            std::string s = addr->toString();
            if(!s.empty() && s[0] == '[') {
                auto end = s.find(']');
                if(end != std::string::npos) {
                    return s.substr(1, end - 1);
                }
            }
        }
    }
    return addr->toString();
}

} // namespace gateway
} // namespace bronx
