#pragma once

// 两个中间件，路由和代理终结。
// 路由中间件调 Router::match 填 ctx.route()，没命中就短路 404。
// 代理终结在链末端，调 Upstream::forward 转发，失败映射成 502 503 504。

#include "mw.h"
#include "router.h"
#include "ups_group.h"
#include "ip.h"
#include <vector>

namespace bronx {
namespace gateway {

// 路由中间件:命中填 route 并放行;未命中短路 404。
Middleware::ptr MakeRouterMiddleware(Router::ptr router, UpstreamRegistry::ptr upstreams);

// 代理终结中间件:转发到 ctx.route() 选定的上游。
// connectTimeoutMs/recvTimeoutMs:上游连接/响应超时。trusted:可信代理名单。
Middleware::ptr MakeProxyMiddleware(uint64_t connectTimeoutMs = 3000,
                                    uint64_t recvTimeoutMs = 30000,
                                    std::vector<bronx::ipban::Ip> trusted = {});

} // namespace gateway
} // namespace bronx
