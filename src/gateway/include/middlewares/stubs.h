#pragma once

// 几个先埋着的接口，这些跟业务和 DB 强相关，网关只给接口和最小默认实现。
// 以后按需换成真的，查 DB Redis 鉴权服务之类，中间件链骨架不用动。

#include "mw.h"
#include <functional>
#include <cstdint>
#include <string>
#include <memory>

namespace bronx {
namespace gateway {

// 认证结果, Ok 放行, 其余都是拒。分开是为了 metrics 和日志能看清咋失败的。
enum class AuthErr {
    Ok,        // 验过了
    NoToken,   // 没带凭证
    BadToken,
    BadKey,
    BadSig,    // 签名不对 或 密钥/算法没配
    Expired,   // 过期了
    BadIss,    // issuer 不符
    BadAud,    // audience 不符
    BadAlg,    // 算法不认 (含 alg=none)
    Scope,
    Role,
    BadPolicy,
};

// 鉴权接口，判请求放不放行，默认全放。以后换成查 token JWT 或调鉴权服务。
class Authenticator {
public:
    using ptr = std::shared_ptr<Authenticator>;
    virtual ~Authenticator() = default;
    // 返回 Ok 放行, 其余短路 401。验过会往 ctx 写 jwt_sub 等。
    virtual AuthErr authenticate(ReqCtx& ctx) = 0;
};
// 默认放行实现
class AllowAllAuthenticator : public Authenticator {
public:
    AuthErr authenticate(ReqCtx&) override { return AuthErr::Ok; }
};
// 鉴权中间件:失败 → 401 短路。
Middleware::ptr MakeAuthMiddleware(Authenticator::ptr auth);

// 响应缓存接口，以后做成带 TTL LRU 的，或者接 Redis。
class ResponseCache {
public:
    using ptr = std::shared_ptr<ResponseCache>;
    virtual ~ResponseCache() = default;
    // 命中返回缓存的完整响应字节(含头+体);未命中返回空。
    virtual bool get(const std::string& key, std::string& out) = 0;
    virtual void put(const std::string& key, const std::string& value) = 0;
};

// WS 透明隧道，Upgrade 之后在 client 和上游之间架一条双向字节隧道。
// 排在限流之后 Proxy 之前，普通 HTTP 直接放，WebSocket 升级请求由它终结，
// 握手转给上游，101 成功后双泵透传。
Middleware::ptr MakeWebSocketTunnelStub(uint64_t connectMs = 3000,
                                        uint64_t idleMs = 60000);

} // namespace gateway
} // namespace bronx
