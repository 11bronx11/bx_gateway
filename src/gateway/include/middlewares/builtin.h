#pragma once

// 内置中间件的工厂函数，CORS，安全头，request-id，健康检查，访问日志，
// 限流，IP 过滤，per-route 鉴权都在这。每个 MakeXxx 造一个中间件塞进链里。

#include "mw.h"
#include "middlewares/stubs.h"
#include "ip.h"
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bronx {
namespace ipban { class Reporter; }
namespace gateway {

// CORS
struct CorsOptions {
    bool        enabled = true;
    std::string allowOrigin = "*";
    std::vector<std::string> allowOrigins;
    std::string allowMethods = "GET, POST, PUT, DELETE, OPTIONS";
    std::string allowHeaders = "Content-Type, Authorization";
    bool        allowCredentials = false;
    int         maxAge = 86400;
    bool        shortCircuitPreflight = true;
};
Middleware::ptr MakeCorsMiddleware(const CorsOptions& opts = CorsOptions());
Middleware::ptr MakeCorsPrepareMiddleware(const CorsOptions& opts = CorsOptions());
Middleware::ptr MakeCorsPreflightMiddleware();

// 重定向
struct RedirectRule {
    std::string fromPrefix;
    std::string toLocation;
    int         status = 302;
};
Middleware::ptr MakeRedirectMiddleware(const std::vector<RedirectRule>& rules);

// 安全响应头
struct SecurityHeadersOptions {
    std::string contentSecurityPolicy = "default-src 'self'";
    std::string strictTransportSecurity = "max-age=31536000";
    std::string frameOptions = "DENY";
    std::string contentTypeOptions = "nosniff";
    std::string referrerPolicy = "no-referrer";
};
Middleware::ptr MakeSecurityHeadersMiddleware(const SecurityHeadersOptions& opts = SecurityHeadersOptions());

// request-id
Middleware::ptr MakeRequestIdMiddleware(const std::string& headerName = "X-Request-Id");
Middleware::ptr MakeClientAddrMiddleware(std::vector<bronx::ipban::Ip> trustedProxies = {});
Middleware::ptr MakeFinishMiddleware();

// 维护开关,全局一个原子标志。admin 一键开关,中间件读它决定放不放行。
class MaintGate {
public:
    static MaintGate& instance() { static MaintGate g; return g; }
    bool on() const { return m_on.load(); }
    void set(bool v) { m_on.store(v); }
private:
    std::atomic<bool> m_on{false};
};

// 维护模式,开着就全站短路 503 + Retry-After。挂 accesslog 内层,让 503 进日志。
struct MaintOptions {
    int         retryAfterSec = 30;
    std::string body = "Service Unavailable\n";
};
Middleware::ptr MakeMaintenanceMiddleware(const MaintOptions& opts = MaintOptions());

// 健康检查
Middleware::ptr MakeHealthCheckMiddleware(const std::string& path = "/healthz");

// 访问日志，纯文本那个是旧接口，JSON 那个是现在用的
Middleware::ptr MakeAccessLogMiddleware();
Middleware::ptr MakeStructuredAccessLogMiddleware(std::vector<bronx::ipban::Ip> trustedProxies = {});

// 令牌桶
class TokenBucket {
public:
    TokenBucket(double capacity, double refillPerSec);
    bool tryAcquire(double n = 1.0);
    bool tryAcquire(double n, uint32_t* retryAfterSec);
private:
    void refill_locked(uint64_t now);
    uint32_t retry_after_locked(double n) const;

    double   m_capacity;
    double   m_refill_per_sec;
    double   m_tokens;
    uint64_t m_lastMs;
    std::mutex m_mutex;
};

// 全局单桶限流，旧接口留着兼容
Middleware::ptr MakeRateLimitMiddleware(double capacity, double refillPerSec);

// 限流触发举报的配置。同一 IP 在 windowMs 窗口里被 429 挡了 hits 次, 就报一条给 daemon,
// banMs 是建议封多久。off 就只限流不报, 行为跟以前一样。
struct RateBanCfg {
    bool     on      = false;
    uint32_t hits    = 20;         // 窗口内攒够这么多次 429 才报
    uint64_t windowMs = 60000;     // 计数窗口
    uint64_t banMs   = 300000;     // 建议封 5 分钟
};

// per-route 限流，读这条路由自己的限流配置。
// key 是 user 就按 jwt_sub 算，没有就退回 client IP，是 ip 就直接按 IP。
// 每个 route 加 key 值一个独立令牌桶。
// trustedProxies=可信代理名单, 限流按真实客户端 IP 计数(和名单同一套解析), 空则只信 TCP peer。
// reporter+cfg: 配了就在同 IP 反复超限时上报坏 IP, 攒够阈值报一次; 传空 reporter 则不报。
Middleware::ptr MakePerRouteRateLimitMiddleware(std::vector<bronx::ipban::Ip> trustedProxies = {},
                                                std::shared_ptr<bronx::ipban::Reporter> reporter = nullptr,
                                                RateBanCfg banCfg = {},
                                                size_t bucketMax = 20000);

// IP 过滤，denylist 命中就 403，allowlist 没命中就 403。
// CIDR 支持 IPv4 精确 IP 和 a.b.c.d/prefix。
struct IPFilterConfig {
    bool                     enabled = false;
    std::string              mode    = "denylist"; // "allowlist" | "denylist"
    std::vector<std::string> cidrs;
};
Middleware::ptr MakeIPFilterMiddleware(const IPFilterConfig& cfg);

// per-route 鉴权，按路由的 authPolicy 走：inherit none jwt api_key。
// inherit 看全局开没开 jwt，api_key 从指定头读，匹配任一个配置的 key 就放行。
// 认证过了再按路由的 reqScopes/reqRoles 查授权, 不够就 403。
// fwdUser 开着就往上游注入 X-User-* 身份头, 注入前先剥掉客户端自带的同名头防伪造。
struct ApiKey {
    std::string id;
    std::string hash;
    bool enabled = true;
    std::vector<std::string> scopes;
};

struct RouteAuthConfig {
    bool                     globalJwtEnabled = false;
    std::string              apiKeyHeader = "X-API-Key";
    std::vector<std::string> apiKeys;
    std::vector<ApiKey>      keys;
    bool                     fwdUser = false;        // 透传身份到上游
    bool                     stripBearer = true;
    bool                     stripApiKey = true;
    std::string              fwdPrefix = "X-User-";  // 注入头前缀
};
Middleware::ptr MakeRouteAuthMiddleware(Authenticator::ptr jwtAuth,
                                        const RouteAuthConfig& cfg);

} // namespace gateway
} // namespace bronx
