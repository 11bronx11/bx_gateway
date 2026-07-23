#pragma once

// 网关侧的 IP 名单中间件, 捕一个 Guard, 请求期只读快照判一次。
// DENY 就 403 短路并标关连接(不让 keep-alive 复用), ALLOW 放行。
// 客户端 IP 走可信代理解析: peer 不是可信代理就用 peer(伪造 XFF 无效), 是才信 XFF。
// 名单为空=没配代理, 只信 TCP peer。

#include "guard.h"
#include "mw.h"
#include "ip.h"
#include <vector>

namespace bronx {
namespace ipban {

// 造一个名单中间件。guard 由 GatewayServer 持有, reload 时同一个 guard 再传进来。
// trustedProxies=可信代理 CIDR 名单, 空则只信 TCP peer。
bronx::gateway::Middleware::ptr MakeBanMiddleware(Guard::ptr guard,
                                                  std::vector<Ip> trustedProxies = {});

} // namespace ipban
} // namespace bronx
