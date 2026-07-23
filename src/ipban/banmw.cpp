#include "banmw.h"
#include "ctx.h"
#include "metrics.h"
#include "log.h"

namespace bronx {
namespace ipban {

using namespace bronx::gateway;

static bronx::BxLogger::ptr g_log = BRONX_LOG_NAME("system");

bronx::gateway::Middleware::ptr MakeBanMiddleware(Guard::ptr guard,
                                                  std::vector<Ip> trustedProxies) {
    return std::make_shared<FuncMiddleware>(
        [guard, trusted = std::move(trustedProxies)](ReqCtx& ctx, const NextFn& next) {
            if(!guard || !guard->enabled()) { next(); return; }
            if(!loadClientAddr(ctx, trusted)) {
                // 地址不清楚, 直接挡
                BRONX_LOG_WARN(g_log) << "ipban: client addr unresolved, deny";
                auto& metrics = GatewayMetrics::instance();
                metrics.incr_ipban_denied();
                metrics.note_deny(6, true);
                reply(ctx, 403, "Forbidden\n", {}, true);
                return;
            }
            const Ip& ip = ctx.clientAddr().client;
            Decision d = guard->eval(ip);
            if(d.action == Act::DENY) {
                auto& metrics = GatewayMetrics::instance();
                metrics.incr_ipban_denied();
                metrics.note_deny(static_cast<size_t>(d.src), false);
                BRONX_LOG_WARN(g_log) << "ipban deny ip=" << ip.toString()
                    << " rule=" << d.ruleId << " src=" << srcName(d.src)
                    << " reason=" << d.reason;
                reply(ctx, 403, "Forbidden\n", {}, true);
                return;
            }
            next();
        }, "ipban");
}

} // namespace ipban
} // namespace bronx
