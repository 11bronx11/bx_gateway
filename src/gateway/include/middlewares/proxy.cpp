#include "middlewares/proxy.h"
#include "upstream.h"
#include "ups_group.h"
#include "conn.h"
#include "http_msg.h"
#include "log.h"

namespace bronx {
namespace gateway {

static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

Middleware::ptr MakeRouterMiddleware(Router::ptr router, UpstreamRegistry::ptr upstreams) {
    return std::make_shared<FuncMiddleware>(
        [router, upstreams](ReqCtx& ctx, const NextFn& next) {
            if(!router) {
                reply(ctx, 404);
                return;
            }
            if(!upstreams) {
                reply(ctx, 503);
                return;
            }
            if(!router->match(ctx, *upstreams)) {
                reply(ctx, 404);
                return;
            }
            if(!ctx.route().upstream) {
                BRONX_LOG_WARN(g_logger) << "route[" << ctx.route().routeKey
                                          << "] matched but upstream group is unavailable";
                reply(ctx, 503);
                return;
            }
            next();   // 命中,放行到下游(通常是代理终结)
        }, "router");
}

Middleware::ptr MakeProxyMiddleware(uint64_t connectTimeoutMs, uint64_t recvTimeoutMs,
                                    std::vector<bronx::ipban::Ip> trusted) {
    return std::make_shared<FuncMiddleware>(
        [connectTimeoutMs, recvTimeoutMs, trusted = std::move(trusted)](ReqCtx& ctx, const NextFn&) {
            // 终结中间件:不调 next
            if(!ctx.route().matched) {
                reply(ctx, 404);
                return;
            }
            if(!ctx.connection() || !ctx.route().upstream) {
                reply(ctx, 503);
                return;
            }
            ForwardResult r = Upstream::forward(ctx, connectTimeoutMs, recvTimeoutMs, trusted);
            switch(r) {
                case ForwardResult::OK: break;   // 已流式回写客户端
                case ForwardResult::CONNECT_FAIL:
                case ForwardResult::SEND_FAIL:
                case ForwardResult::BAD_RESPONSE:
                    reply(ctx, 502); break;
                case ForwardResult::UPSTREAM_TIMEOUT:
                    reply(ctx, 504); break;
                case ForwardResult::UPSTREAM_BUSY:
                    reply(ctx, 503); break;
                case ForwardResult::BAD_REQUEST_BODY:
                    reply(ctx, 400); break;
                case ForwardResult::REQUEST_TOO_LARGE:
                    reply(ctx, 413); break;
                case ForwardResult::EXPECT_FAIL:
                    if(ctx.request()) ctx.request()->setClose(true);
                    reply(ctx, 417); break;
            }
        }, "proxy");
}

} // namespace gateway
} // namespace bronx
