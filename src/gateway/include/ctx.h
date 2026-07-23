#pragma once

// 连接层和中间件层之间的接缝，一次请求的共享状态都挂这。
// 中间件读 request 写 response，路由结果放 route()，跨中间件传值用 setAttr getAttr。

#include "http_msg.h"
#include "body.h"
#include "ip.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <any>

namespace bronx {
namespace gateway {

class GatewayConnection;
class UpstreamGroup;
struct RouteRule;

// 路由结果：匹配后写入 ctx.route()
struct RouteResult {
    bool                            matched     = false;
    std::shared_ptr<RouteRule>      rule;       // 命中的路由规则（含所有配置）
    std::shared_ptr<UpstreamGroup>  upstream;   // 已解析的 UpstreamGroup
    std::string                     routeKey;   // 命中的 path/key（调试/日志）
};

struct ClientAddr {
    bronx::ipban::Ip peer;
    bronx::ipban::Ip client;
    bool ok = false;
};

enum class RespState {
    OPEN,
    SENT,
    STREAM,
    TUNNEL,
    WRITE_FAIL
};

class ReqCtx {
public:
    explicit ReqCtx(GatewayConnection* conn);
    explicit ReqCtx(GwRequest::ptr req);

    GwRequest::ptr      request()     const;
    BodyReader::ptr requestBody() const;
    GatewayConnection*  connection()  const { return m_conn; }

    GwResponse::ptr     response()    const { return m_response; }
    void setResponse(const GwResponse::ptr& r) { m_response = r; }

    const std::string& responseBody() const { return m_resp_body; }
    void setResponseBody(const std::string& b) { m_resp_body = b; }

    bool isHandled() const { return m_handled; }
    void setHandled(bool v) { m_handled = v; }

    ClientAddr& clientAddr() { return m_client_addr; }
    const ClientAddr& clientAddr() const { return m_client_addr; }
    bool clientAddrReady() const { return m_client_addr_ready; }
    void setClientAddr(const ClientAddr& v) { m_client_addr = v; m_client_addr_ready = true; }

    RespState respState() const { return m_resp_state; }
    uint64_t startMs() const { return m_start_ms; }
    uint64_t commitMs() const { return m_commit_ms; }
    int status() const { return m_status; }
    bool commitResp(RespState state, int status, uint64_t now);

    RouteResult&       route()       { return m_route; }
    const RouteResult& route() const { return m_route; }

    void add_resp_hdr(const std::string& k, const std::string& v);
    const HeaderMap& extra_resp_hdrs() const { return m_extra_resp_hdrs; }

    void setAttr(const std::string& k, std::any v) { m_attrs[k] = std::move(v); }
    template<typename T>
    bool getAttr(const std::string& k, T& out) const {
        auto it = m_attrs.find(k);
        if(it == m_attrs.end()) return false;
        try { out = std::any_cast<T>(it->second); return true; }
        catch(...) { return false; }
    }

private:
    GatewayConnection*              m_conn;
    GwRequest::ptr                  m_req_override;
    GwResponse::ptr                 m_response;
    std::string                     m_resp_body;
    bool                            m_handled = false;
    ClientAddr                      m_client_addr;
    bool                            m_client_addr_ready = false;
    RespState                       m_resp_state = RespState::OPEN;
    uint64_t                        m_start_ms = 0;
    uint64_t                        m_commit_ms = 0;
    int                             m_status = 0;
    RouteResult                     m_route;
    HeaderMap                       m_extra_resp_hdrs;
    std::map<std::string, std::any> m_attrs;
};

} // namespace gateway
} // namespace bronx
