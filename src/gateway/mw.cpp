#include "ctx.h"
#include "mw.h"
#include "conn.h"
#include "str.h"
#include "http_msg.h"
#include "net_socket.h"
#include "endpoint.h"
#include "metrics.h"
#include "util.h"
#include <utility>
#include <strings.h>

namespace bronx {
namespace gateway {

// ReqCtx
ReqCtx::ReqCtx(GatewayConnection* conn)
    : m_conn(conn)
    , m_start_ms(bronx::GetCurrentMs()) {
}

ReqCtx::ReqCtx(GwRequest::ptr req)
    : m_conn(nullptr)
    , m_req_override(std::move(req))
    , m_start_ms(bronx::GetCurrentMs()) {
}

bool ReqCtx::commitResp(RespState state, int status, uint64_t now) {
    if(m_resp_state != RespState::OPEN) {
        if(state != RespState::WRITE_FAIL || m_resp_state == RespState::WRITE_FAIL) {
            return false;
        }
        m_resp_state = RespState::WRITE_FAIL;
        if(m_status == 0) m_status = status;
        return true;
    }
    m_resp_state = state;
    m_status = status;
    m_commit_ms = now;
    return true;
}

GwRequest::ptr ReqCtx::request() const {
    if(m_req_override) {
        return m_req_override;
    }
    if(!m_conn) {
        return nullptr;
    }
    return m_conn->request();
}

BodyReader::ptr ReqCtx::requestBody() const {
    if(!m_conn) {
        return nullptr;
    }
    return m_conn->requestBody();
}

void ReqCtx::add_resp_hdr(const std::string& k, const std::string& v) {
    if(strcasecmp(k.c_str(), "Vary") == 0) {
        auto it = m_extra_resp_hdrs.find("Vary");
        if(it == m_extra_resp_hdrs.end() || trim_ascii(it->second).empty()) {
            m_extra_resp_hdrs[k] = v;
        } else if(!hdr_has_token(it->second, v)) {
            it->second += ", ";
            it->second += v;
        }
        return;
    }
    m_extra_resp_hdrs[k] = v;
}

// MwChain
// 洋葱链:递归构造 next。索引 i 的中间件的 next 调用 i+1。
// 短路:某中间件不调 next,链就停在那里(后续不执行)。
void MwChain::run(ReqCtx& ctx) const {
    // 用 std::function 递归
    std::function<void(size_t)> dispatch = [&](size_t i) {
        if(i >= m_mws.size()) {
            return;   // 链尾:无更多中间件
        }
        // 已被短路标记则不再深入(防御性:正常短路中间件不会调 next)
        if(ctx.isHandled()) {
            return;
        }
        NextFn next = [&dispatch, i]() { dispatch(i + 1); };
        m_mws[i]->handle(ctx, next);
    };
    dispatch(0);
}

bool loadClientAddr(ReqCtx& ctx, const std::vector<bronx::ipban::Ip>& trusted) {
    if(ctx.clientAddrReady()) {
        return ctx.clientAddr().ok;
    }
    ClientAddr out;
    auto conn = ctx.connection();
    auto sock = conn ? conn->socket() : nullptr;
    auto addr = sock ? sock->getRemoteAddress() : nullptr;
    if(addr && bronx::ipban::ipFromSockaddr(addr->getAddr(), out.peer)) {
        std::string xff;
        if(auto req = ctx.request()) {
            xff = req->getHeader("X-Forwarded-For");
        }
        out.ok = bronx::ipban::resolveClientAddr(out.peer, xff, trusted, out.client);
    }
    ctx.setClientAddr(out);
    return out.ok;
}

void applyHeaders(ReqCtx& ctx, const GwResponse::ptr& rsp) {
    if(!rsp) {
        return;
    }
    for(const auto& h : ctx.extra_resp_hdrs()) {
        rsp->setHeader(h.first, h.second);
    }
}

bool reply(ReqCtx& ctx, int status, const std::string& body,
           const HeaderMap& headers, bool close) {
    if(ctx.respState() != RespState::OPEN) {
        return false;
    }
    auto rsp = std::make_shared<GwResponse>();
    rsp->setStatus(status);
    applyHeaders(ctx, rsp);
    for(const auto& h : headers) {
        rsp->setHeader(h.first, h.second);
    }
    if(!body.empty() && !rsp->hasHeader("Content-Type")) {
        rsp->setHeader("Content-Type", "text/plain");
    }
    ctx.setResponse(rsp);
    ctx.setResponseBody(body);
    ctx.setHandled(true);
    if(close && ctx.request()) {
        ctx.request()->setClose(true);
    }
    int sent = ctx.connection() ? ctx.connection()->sendResponse(rsp, body) : 1;
    if(sent <= 0) {
        writeFail(ctx);
        return false;
    }
    ctx.commitResp(RespState::SENT, rsp->getStatus(), bronx::GetCurrentMs());
    return true;
}

bool reply(ReqCtx& ctx, int status) {
    return reply(ctx, status, std::string(HttpStatusReason(status)) + "\n");
}

void writeFail(ReqCtx& ctx) {
    GatewayMetrics::instance().incrWriteFail();
    if(ctx.connection()) {
        ctx.connection()->markWriteFail(ctx.response() ? ctx.response()->getStatus() : 0);
    }
    ctx.commitResp(RespState::WRITE_FAIL, ctx.response() ? ctx.response()->getStatus() : 0,
                   bronx::GetCurrentMs());
    ctx.setHandled(true);
    if(ctx.request()) {
        ctx.request()->setClose(true);
    }
}

} // namespace gateway
} // namespace bronx
