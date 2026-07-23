#pragma once

// 路由匹配，按 path method host 三个维度，分精确和前缀两类。
// 精确优先于前缀，同类里再按 host method priority 和插入顺序比。
// match 命中就把 rule 和 upstream 填进 ctx.route()，reload 时整张表替换。
// 一条 RouteRule 带齐这条路由的所有配置，改前缀，改头，限流，鉴权策略。

#include "ctx.h"
#include "rt_cache.h"
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <atomic>
#include <functional>
#include <mutex>

namespace bronx {
namespace gateway {

class UpstreamRegistry;

enum class MatchType { EXACT, PREFIX };

// 一条路由规则（Router 拥有，ctx.route().rule 持 shared_ptr）
struct RouteRule {
    std::string  name;
    MatchType    matchType   = MatchType::PREFIX;
    std::string  pathPattern;
    std::vector<std::string> methods;   // 空=any
    std::string  host;                  // 空=any
    std::string  upstream;              // UpstreamGroup name

    bool         stripPrefix   = false;
    std::string  rewritePrefix;         // M9：strip 后再加此前缀

    int          priority      = 0;
    uint32_t     insertionIdx  = 0;     // 填充时赋，用于稳定 tie-break

    // M9：请求 header 改写（set 在 strip 前，remove 在 set 后）
    std::map<std::string, std::string> reqHeaderSet;
    std::vector<std::string>           reqHeaderRemove;

    // M7：per-route 限流（0 = 不限流）
    bool     rateLimitEnabled    = false;
    double   rateLimitCapacity   = 0;
    double   rateLimitRefillPerSec = 0;
    std::string rateLimitKey;           // "user"(jwt_sub) | "ip"

    // Per-route 鉴权策略："inherit" 继承全局，"none" 公开，
    // "jwt" 使用 JWT，"api_key" 使用 X-API-Key。
    std::string authPolicy = "inherit";

    // 授权要求, 认证过了再查这俩。空=该维度不设限。
    // scope 全都要有(ALL), role 命中任一即可(ANY), 两边都过才放行。
    std::vector<std::string> reqScopes;
    std::vector<std::string> reqRoles;

    // M8：IP filter（由全局 IPFilter 中间件执行；route 级 override 预留）
    // 本轮不在 RouteRule 级别做，由全局中间件处理。
};

using KeyExtractor = std::function<std::string(const ReqCtx&)>;

// 路由表：不可修改（reload 时整表替换），线程安全读
class RouteTable {
public:
    // 添加规则（按插入顺序，建完后调 build() 完成内部索引）
    void addRule(RouteRule rule);
    void build();           // 排序 prefixVec；必须在 match 前调用

    // 匹配：命中返回 shared_ptr<RouteRule>，未命中返回 nullptr
    std::shared_ptr<RouteRule> match(const std::string& method,
                                     const std::string& host,
                                     const std::string& path) const;
    size_t ruleCount() const { return m_rules.size(); }
    std::vector<RouteRule> listRules() const;

private:
    bool ruleMatches(const RouteRule& r, const std::string& method,
                     const std::string& host, const std::string& path) const;
    int  compareRules(const RouteRule& a, const RouteRule& b) const;

    std::vector<std::shared_ptr<RouteRule>>          m_rules;
    std::unordered_map<std::string,
        std::vector<size_t>>                         m_exactMap; // path→rule indices
    std::vector<size_t>                              m_prefixVec;// sorted
};

class Router {
public:
    using ptr = std::shared_ptr<Router>;

    Router();

    // 构建路由表（配置加载时调用）
    void setTable(std::shared_ptr<RouteTable> table);

    // 匹配：填 ctx.route()（rule + upstream）。成功返回 true。
    // upstreams 用于把 rule.upstream（名字）解析成 UpstreamGroup::ptr。
    bool match(ReqCtx& ctx,
               const UpstreamRegistry& upstreams) const;

    // 传 nullptr 时关缓存,其余情况替换默认 ARC
    void setCache(RouteCache::ptr cache) {
        std::lock_guard lk(m_pending_mtx);
        m_cache = cache ? std::move(cache) : std::make_shared<NullRouteCache>();
    }

    // generation：每次 setTable 自动 +1，使旧 cache key 自然 miss
    uint32_t generation() const { return m_gen.load(std::memory_order_relaxed); }

    // cache 命中/miss 统计（供 /stats 使用）
    void cacheStats(uint64_t& hits, uint64_t& misses) const {
        RouteCache::ptr c;
        { std::lock_guard lk(m_pending_mtx); c = m_cache; }
        if(c) c->stats(hits, misses);
        else hits = misses = 0;
    }

    size_t routeCount() const;
    std::vector<RouteRule> listRoutes() const;

    // 兼容旧接口：单条添加（内部 build RouteTable）
    void addRoute(RouteRule rule);
    void clearRoutes();

    // KeyExtractor 升级扩展点：默认=path；推理网关换成 model
    void setKeyExtractor(KeyExtractor ke);

private:
    std::shared_ptr<RouteTable> m_table;
    RouteCache::ptr             m_cache;
    std::atomic<uint32_t>       m_gen{0};
    KeyExtractor                m_key_extractor;

    // 兼容旧接口时用的可变积累列表
    std::vector<RouteRule>      m_pending;
    mutable std::mutex          m_pending_mtx;
};

} // namespace gateway
} // namespace bronx
