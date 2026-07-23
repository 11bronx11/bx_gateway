#include "conf.h"
#include "str.h"
#include "ups_group.h"
#include "mw.h"
#include "middlewares/builtin.h"
#include "middlewares/proxy.h"
#include "middlewares/stubs.h"
#include "jwt.h"
#include "log.h"
#include "reactor.h"
#include "guard.h"
#include "banmw.h"
#include "middlewares/waf.h"
#include "report.h"
#include "arc_cache.h"
#include <yaml-cpp/yaml.h>
#include <stdexcept>
#include <algorithm>
#include <cstdint>
#include <cctype>
#include <limits>
#include <unordered_set>

namespace bronx {
namespace gateway {

static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

static uint32_t clampPositiveU32(const YAML::Node& parent,
                                 const char* key,
                                 uint32_t current,
                                 const std::string& logPrefix,
                                 uint32_t minValue = 1,
                                 uint32_t maxValue = UINT32_MAX) {
    if(!parent || !parent[key]) {
        return current;
    }
    int64_t raw = parent[key].as<int64_t>();
    if(raw < (int64_t)minValue) {
        BRONX_LOG_WARN(g_logger) << logPrefix << "." << key << "=" << raw
                                  << ", clamp to " << minValue;
        return minValue;
    }
    if(raw > (int64_t)maxValue) {
        BRONX_LOG_WARN(g_logger) << logPrefix << "." << key << "=" << raw
                                  << ", clamp to " << maxValue;
        return maxValue;
    }
    return (uint32_t)raw;
}

static std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string norm_jwt_algo(const std::string& raw) {
    std::string algo = lower_ascii(trim_ascii(raw));
    if(algo == "hs256") return "HS256";
    if(algo == "rs256") return "RS256";
    if(algo == "es256") return "ES256";
    return "";
}

static bool is_known_auth(const std::string& policy) {
    return policy == "inherit" || policy == "none"
        || policy == "jwt" || policy == "api_key";
}

static bool read_needs(const YAML::Node& node, std::vector<std::string>& out) {
    if(!node) return true;
    if(!node.IsSequence()) return false;
    try {
        for(const auto& item : node) {
            if(!item.IsScalar()) return false;
            std::string val = item.as<std::string>();
            if(val.empty()) return false;
            out.push_back(val);
        }
    } catch(...) {
        return false;
    }
    return true;
}

static CorsOptions parse_cors(const YAML::Node& root) {
    CorsOptions opts;
    if(!root || !root["cors"]) {
        return opts;
    }
    const auto& c = root["cors"];
    if(c["enabled"]) opts.enabled = c["enabled"].as<bool>();
    if(c["allow_origin"]) opts.allowOrigin = c["allow_origin"].as<std::string>();
    if(c["allow_origins"] && c["allow_origins"].IsSequence()) {
        opts.allowOrigins.clear();
        for(const auto& o : c["allow_origins"]) {
            if(o.IsScalar()) {
                std::string v = trim_ascii(o.as<std::string>());
                if(!v.empty()) opts.allowOrigins.push_back(v);
            }
        }
    }
    if(c["allow_methods"]) opts.allowMethods = c["allow_methods"].as<std::string>();
    if(c["allow_headers"]) opts.allowHeaders = c["allow_headers"].as<std::string>();
    if(c["allow_credentials"]) opts.allowCredentials = c["allow_credentials"].as<bool>();
    opts.maxAge = (int)clampPositiveU32(c, "max_age", (uint32_t)opts.maxAge, "cors", 0,
                                        (uint32_t)std::numeric_limits<int>::max());
    if(c["short_circuit_preflight"]) {
        opts.shortCircuitPreflight = c["short_circuit_preflight"].as<bool>();
    }
    if(opts.allowCredentials && opts.allowOrigin == "*" && opts.allowOrigins.empty()) {
        BRONX_LOG_WARN(g_logger) << "cors.allow_credentials=true with wildcard origin; "
                                 << "runtime will echo request Origin when present";
    }
    return opts;
}

// ── 兼容旧接口（不做删除）────────────────────────────────────────────────────

Router::ptr GatewayConfig::LoadRouterFromYaml(const YAML::Node& node) {
    auto snap = BuildSnapshotFromYaml(node);
    return snap ? snap->router : nullptr;
}

Router::ptr GatewayConfig::LoadRouterFromFile(const std::string& path) {
    auto snap = BuildSnapshot(path);
    return snap ? snap->router : nullptr;
}

// ── Round 2 全量加载 ────────────────────────────────────────────────────────

static UpstreamRegistry::ptr parse_upstreams(const YAML::Node& node) {
    auto reg = std::make_shared<UpstreamRegistry>();
    if(!node || !node.IsSequence()) return reg;
    for(const auto& u : node) {
        std::string name = u["name"] ? u["name"].as<std::string>() : "";
        if(name.empty()) continue;
        std::string lbType = u["lb"] ? u["lb"].as<std::string>() : "round_robin";
        if(!IsSupportedLoadBalancer(lbType)) {
            BRONX_LOG_WARN(g_logger) << "upstream[" << name << "] unsupported lb="
                                      << lbType << ", fallback to round_robin";
            lbType = "round_robin";
        }

        CircuitBreakerConfig cbCfg;
        ConnPoolConfig poolCfg;
        uint32_t maxInflight = 0;
        uint32_t totalMs = 30000;
        uint32_t connectMs = 3000;
        uint32_t readMs = 30000;
        if(u["timeout"]) {
            const auto& t = u["timeout"];
            std::string prefix = "upstream[" + name + "] timeout";
            totalMs = clampPositiveU32(t, "total_ms", totalMs, prefix);
            connectMs = clampPositiveU32(t, "connect_ms", connectMs, prefix);
            readMs = clampPositiveU32(t, "read_ms", readMs, prefix);
        }
        if(u["limits"]) {
            const auto& l = u["limits"];
            std::string prefix = "upstream[" + name + "] limits";
            maxInflight = clampPositiveU32(
                l, "max_inflight", maxInflight, prefix, 0,
                (uint32_t)std::numeric_limits<int32_t>::max());
        }
        if(u["circuit_breaker"]) {
            const auto& cb = u["circuit_breaker"];
            std::string prefix = "upstream[" + name + "] circuit_breaker";
            cbCfg.failureThreshold = clampPositiveU32(
                cb, "failure_threshold", cbCfg.failureThreshold, prefix);
            cbCfg.windowMs = clampPositiveU32(
                cb, "window_ms", cbCfg.windowMs, prefix);
            cbCfg.buckets = clampPositiveU32(
                cb, "buckets", cbCfg.buckets, prefix, 1, 1024);
            cbCfg.minRequests = clampPositiveU32(
                cb, "min_requests", cbCfg.minRequests, prefix);
            cbCfg.failureRate = clampPositiveU32(
                cb, "failure_rate", cbCfg.failureRate, prefix, 0, 100);
            cbCfg.slowMs = clampPositiveU32(
                cb, "slow_ms", cbCfg.slowMs, prefix);
            cbCfg.slowRate = clampPositiveU32(
                cb, "slow_rate", cbCfg.slowRate, prefix, 0, 100);
            cbCfg.openTimeoutMs = clampPositiveU32(
                cb, "open_timeout_ms", cbCfg.openTimeoutMs, prefix);
            cbCfg.maxOpenTimeoutMs = clampPositiveU32(
                cb, "max_open_timeout_ms", cbCfg.maxOpenTimeoutMs, prefix,
                cbCfg.openTimeoutMs);
            cbCfg.halfOpenMaxRequests = clampPositiveU32(
                cb, "half_open_max_requests", cbCfg.halfOpenMaxRequests, prefix);
            cbCfg.halfOpenSuccesses = clampPositiveU32(
                cb, "half_open_successes", cbCfg.halfOpenSuccesses, prefix,
                1, cbCfg.halfOpenMaxRequests);
            if(cb["failure_statuses"] && cb["failure_statuses"].IsSequence()) {
                cbCfg.failureStatuses.clear();
                for(const auto& s : cb["failure_statuses"]) {
                    int status = s.as<int>();
                    if(status >= 100 && status <= 599) {
                        cbCfg.failureStatuses.push_back(status);
                    } else {
                        BRONX_LOG_WARN(g_logger) << prefix << ".failure_statuses="
                                                 << status << ", skip";
                    }
                }
            }
        }
        if(u["connection_pool"]) {
            const auto& p = u["connection_pool"];
            std::string prefix = "upstream[" + name + "] connection_pool";
            poolCfg.maxIdle = clampPositiveU32(
                p, "max_idle", poolCfg.maxIdle, prefix, 0);
            poolCfg.idleTimeoutMs = clampPositiveU32(
                p, "idle_timeout_ms", poolCfg.idleTimeoutMs, prefix);
        }

        auto lb = MakeLoadBalancer(lbType);
        auto group = std::make_shared<UpstreamGroup>(name, lb);
        group->setTimeouts(totalMs, connectMs, readMs);
        if(u["health_check"]) {
            const auto& h = u["health_check"];
            HealthCheckConfig hc;
            hc.enabled = h["enabled"] ? h["enabled"].as<bool>() : false;
            hc.path = h["path"] ? h["path"].as<std::string>() : "/healthz";
            std::string prefix = "upstream[" + name + "] health_check";
            hc.intervalMs = clampPositiveU32(
                h, "interval_ms", hc.intervalMs, prefix);
            hc.timeoutMs = clampPositiveU32(
                h, "timeout_ms", hc.timeoutMs, prefix);
            hc.healthyThreshold = clampPositiveU32(
                h, "healthy_threshold", hc.healthyThreshold, prefix);
            hc.unhealthyThreshold = clampPositiveU32(
                h, "unhealthy_threshold", hc.unhealthyThreshold, prefix);
            group->setHealthCheck(hc);
        }

        if(u["endpoints"] && u["endpoints"].IsSequence()) {
            for(const auto& e : u["endpoints"]) {
                std::string host = e["host"] ? e["host"].as<std::string>() : "";
                uint32_t port    = e["port"] ? e["port"].as<uint32_t>() : 80;
                uint32_t weight  = 1;
                if(e["weight"]) {
                    int64_t rawWeight = e["weight"].as<int64_t>();
                    if(rawWeight <= 0) {
                        BRONX_LOG_WARN(g_logger) << "upstream[" << name
                                                  << "] endpoint weight=" << rawWeight
                                                  << " for " << host << ":" << port
                                                  << ", clamp to 1";
                    } else if(rawWeight > kMaxLoadBalancerWeight) {
                        BRONX_LOG_WARN(g_logger) << "upstream[" << name
                                                  << "] endpoint weight=" << rawWeight
                                                  << " for " << host << ":" << port
                                                  << ", clamp to " << kMaxLoadBalancerWeight;
                        weight = kMaxLoadBalancerWeight;
                    } else {
                        weight = (uint32_t)rawWeight;
                    }
                }
                if(host.empty()) continue;
                auto addr = bronx::BxAddress::ResolveOneIp(host);
                if(!addr) {
                    BRONX_LOG_WARN(g_logger) << "upstream[" << name
                                             << "] skip unresolved endpoint "
                                             << host << ":" << port;
                    continue;
                }
                group->addEndpoint(std::make_shared<Endpoint>(
                    host, port, weight, cbCfg, poolCfg, maxInflight));
            }
        }
        reg->add(group);
        BRONX_LOG_INFO(g_logger) << "upstream[" << name << "] lb=" << lbType
                                  << " endpoints=" << group->endpointCount();
    }
    return reg;
}

static Router::ptr parse_router(const YAML::Node& node, const UpstreamRegistry& reg) {
    auto table = std::make_shared<RouteTable>();
    if(!node || !node.IsSequence()) {
        auto r = std::make_shared<Router>();
        r->setTable(table);
        return r;
    }
    for(const auto& r : node) {
        RouteRule rule;
        rule.name        = r["name"]        ? r["name"].as<std::string>()  : "";
        std::string mt   = lower_ascii(trim_ascii(
            r["match_type"]  ? r["match_type"].as<std::string>() : "prefix"));
        if(mt == "exact") {
            rule.matchType = MatchType::EXACT;
        } else if(mt == "prefix") {
            rule.matchType = MatchType::PREFIX;
        } else {
            BRONX_LOG_WARN(g_logger) << "route[" << rule.name
                                      << "] unsupported match_type=" << mt
                                      << ", skipped";
            continue;
        }
        rule.pathPattern = r["path"]        ? r["path"].as<std::string>() : "/";
        if(rule.pathPattern.empty()) {
            BRONX_LOG_WARN(g_logger) << "route[" << rule.name
                                      << "] empty path, fallback to /";
            rule.pathPattern = "/";
        } else if(rule.pathPattern[0] != '/') {
            BRONX_LOG_WARN(g_logger) << "route[" << rule.name
                                      << "] path=" << rule.pathPattern
                                      << " missing leading '/', prepend";
            rule.pathPattern.insert(rule.pathPattern.begin(), '/');
        }
        rule.upstream    = r["upstream"]    ? r["upstream"].as<std::string>() : "";
        rule.stripPrefix = r["strip_prefix"]? r["strip_prefix"].as<bool>() : false;
        rule.rewritePrefix = r["rewrite_prefix"] ? r["rewrite_prefix"].as<std::string>() : "";
        if(!rule.rewritePrefix.empty() && rule.rewritePrefix[0] != '/') {
            BRONX_LOG_WARN(g_logger) << "route[" << rule.name
                                      << "] rewrite_prefix=" << rule.rewritePrefix
                                      << " missing leading '/', prepend";
            rule.rewritePrefix.insert(rule.rewritePrefix.begin(), '/');
        }
        rule.host        = r["host"]        ? r["host"].as<std::string>() : "";
        rule.priority    = r["priority"]    ? r["priority"].as<int>() : 0;

        if(r["methods"] && r["methods"].IsSequence())
            for(const auto& m : r["methods"]) rule.methods.push_back(m.as<std::string>());

        if(r["request_headers"]) {
            const auto& rh = r["request_headers"];
            if(rh["set"] && rh["set"].IsMap())
                for(const auto& kv : rh["set"]) rule.reqHeaderSet[kv.first.as<std::string>()] = kv.second.as<std::string>();
            if(rh["remove"] && rh["remove"].IsSequence())
                for(const auto& k : rh["remove"]) rule.reqHeaderRemove.push_back(k.as<std::string>());
        }
        if(r["rate_limit"]) {
            const auto& rl = r["rate_limit"];
            rule.rateLimitEnabled      = rl["enabled"]        ? rl["enabled"].as<bool>()     : false;
            rule.rateLimitCapacity     = rl["capacity"]       ? rl["capacity"].as<double>()   : 0;
            rule.rateLimitRefillPerSec = rl["refill_per_sec"] ? rl["refill_per_sec"].as<double>() : 0;
            rule.rateLimitKey          = rl["key"]            ? rl["key"].as<std::string>()   : "ip";
            if(rule.rateLimitCapacity < 0) {
                BRONX_LOG_WARN(g_logger) << "route[" << rule.name
                                          << "] negative rate_limit.capacity clamped to 0";
                rule.rateLimitCapacity = 0;
            }
            if(rule.rateLimitRefillPerSec < 0) {
                BRONX_LOG_WARN(g_logger) << "route[" << rule.name
                                          << "] negative rate_limit.refill_per_sec clamped to 0";
                rule.rateLimitRefillPerSec = 0;
            }
            if(rule.rateLimitKey != "ip" && rule.rateLimitKey != "user") {
                BRONX_LOG_WARN(g_logger) << "route[" << rule.name
                                          << "] unsupported rate_limit.key="
                                          << rule.rateLimitKey << ", fallback to ip";
                rule.rateLimitKey = "ip";
            }
        }
        if(r["auth"]) {
            if(r["auth"].IsScalar()) {
                rule.authPolicy = lower_ascii(trim_ascii(r["auth"].as<std::string>()));
            } else if(r["auth"]["policy"]) {
                rule.authPolicy = lower_ascii(trim_ascii(r["auth"]["policy"].as<std::string>()));
            }
            if(rule.authPolicy.empty()) {
                rule.authPolicy = "inherit";
            } else if(!is_known_auth(rule.authPolicy)) {
                BRONX_LOG_WARN(g_logger) << "route[" << rule.name
                                          << "] unsupported auth policy="
                                          << rule.authPolicy
                                          << ", route will fail closed";
            }
        }
        if(!read_needs(r["require_scopes"], rule.reqScopes)
            || !read_needs(r["require_roles"], rule.reqRoles)) {
            BRONX_LOG_WARN(g_logger) << "route[" << rule.name
                                      << "] bad auth needs, skipped";
            continue;
        }

        if(rule.upstream.empty() || !reg.get(rule.upstream)) {
            BRONX_LOG_WARN(g_logger) << "route[" << rule.name << "]: upstream '"
                                      << rule.upstream << "' not found, skipped";
            continue;
        }
        BRONX_LOG_INFO(g_logger) << "route[" << rule.name << "] " << rule.pathPattern
                                  << " -> " << rule.upstream;
        table->addRule(std::move(rule));
    }
    table->build();
    auto router = std::make_shared<Router>();
    router->setTable(table);
    return router;
}

ConfigSnapshot::ptr GatewayConfig::BuildSnapshotFromYaml(const YAML::Node& root,
                                                        bronx::BxIoManager* iom,
                                                        std::shared_ptr<bronx::ipban::Guard> guard,
                                                        std::shared_ptr<bronx::ipban::Reporter> reporter,
                                                        ipban::StaticPolicy* pending) {
    auto snap = std::make_shared<ConfigSnapshot>();
    ipban::StaticPolicy policy;
    bool hasPolicy = false;

    snap->upstreams = parse_upstreams(root["upstreams"]);
    snap->router    = parse_router(root["routes"], *snap->upstreams);

    // rt_cache 段可覆盖全局默认 ARC 容量
    if(root["rt_cache"]) {
        size_t cap = root["rt_cache"]["cap"] ? root["rt_cache"]["cap"].as<size_t>() : 1024;
        if(cap == 0) throw std::invalid_argument("rt_cache.cap must be greater than zero");
        snap->router->setCache(std::make_shared<ArcRouteCache>(cap));
    }

    // 可信代理名单: 决定 XFF 信不信。名单里的 CIDR 是你自己的前置 nginx/LB 地址。
    // 空 = 没配代理, 名单/限流都只信 TCP peer(直连者伪造 XFF 无效)。
    std::vector<ipban::Ip> trustedProxies;
    if(root["trusted_proxies"] && root["trusted_proxies"].IsSequence()) {
        for(const auto& c : root["trusted_proxies"]) {
            ipban::Ip ip;
            if(ipban::parseCidr(c.as<std::string>(), ip)) trustedProxies.push_back(ip);
            else BRONX_LOG_WARN(g_logger) << "bad trusted_proxies cidr: " << c.as<std::string>();
        }
    }
    CorsOptions cors = parse_cors(root);

    auto chain = std::make_shared<MwChain>();
    chain->use(MakeRequestIdMiddleware());
    chain->use(MakeClientAddrMiddleware(trustedProxies));
    chain->use(MakeStructuredAccessLogMiddleware(trustedProxies));
    chain->use(MakeSecurityHeadersMiddleware());
    chain->use(MakeCorsPrepareMiddleware(cors));
    chain->use(MakeFinishMiddleware());

    // IP Filter / 动态名单
    // 读 ip_filter 段(静态规则)。有 guard 就灌进账本走动态名单中间件,
    // 没 guard(旧调用点/测试)退回老的静态 IPFilter 中间件, 行为不变。
    ipban::StaticFilterCfg sfCfg;
    IPFilterConfig ipCfg;
    if(root["ip_filter"]) {
        const auto& f = root["ip_filter"];
        bool en = f["enabled"] ? f["enabled"].as<bool>() : false;
        std::string mode = f["mode"] ? f["mode"].as<std::string>() : "denylist";
        if(mode != "allowlist" && mode != "denylist") {
            BRONX_LOG_WARN(g_logger) << "unsupported ip_filter.mode="
                                      << mode << ", fallback to denylist";
            mode = "denylist";
        }
        std::vector<std::string> cidrs;
        if(f["cidrs"] && f["cidrs"].IsSequence())
            for(const auto& c : f["cidrs"]) cidrs.push_back(c.as<std::string>());
        sfCfg.enabled = en; sfCfg.mode = mode; sfCfg.cidrs = cidrs;
        ipCfg.enabled = en; ipCfg.mode = mode; ipCfg.cidrs = cidrs;
    }
    ipban::Act def;
    size_t skipped = 0;
    for(const auto& cidr : sfCfg.cidrs) {
        ipban::Ip ip;
        if(!ipban::parseCidr(cidr, ip)) ++skipped;
    }
    if(skipped) {
        BRONX_LOG_ERROR(g_logger) << "ip_filter: " << skipped
                                  << " invalid cidr(s)";
        return nullptr;
    }
    auto rules = ipban::staticRulesFromCfg(sfCfg, def);
    if(guard) {
        policy.enabled = sfCfg.enabled;
        policy.def = def;
        policy.rules = std::move(rules);
        hasPolicy = true;
        chain->use(ipban::MakeBanMiddleware(guard, trustedProxies));
    } else {
        chain->use(MakeIPFilterMiddleware(ipCfg));
    }

    chain->use(MakeHealthCheckMiddleware("/healthz"));
    chain->use(MakeCorsPreflightMiddleware());
    chain->use(MakeMaintenanceMiddleware());

    // waf 全局装在名单后 路由前: 名单最便宜(查一次快照)先挡, 已封的 IP 连正则都不跑;
    // waf 只看原始请求(url + header)不需要路由上下文, 放路由前正确。命中 403 + 报坏 IP。
    ipban::WafCfg wafCfg;
    RateBanCfg rateBanCfg;
    if(root["ip_policy"]) {
        const auto& p = root["ip_policy"];
        if(p["waf"]) {
            const auto& w = p["waf"];
            wafCfg.on    = w["enabled"] ? w["enabled"].as<bool>() : false;
            if(w["ban_ms"]) wafCfg.banMs = w["ban_ms"].as<uint64_t>();
        }
        if(p["rate_report"]) {
            const auto& rr = p["rate_report"];
            rateBanCfg.on = rr["enabled"] ? rr["enabled"].as<bool>() : false;
            if(rr["hits"])      rateBanCfg.hits     = rr["hits"].as<uint32_t>();
            if(rr["window_ms"]) rateBanCfg.windowMs = rr["window_ms"].as<uint64_t>();
            if(rr["ban_ms"])    rateBanCfg.banMs    = rr["ban_ms"].as<uint64_t>();
        }
    }
    if(wafCfg.on) {
        chain->use(ipban::MakeWaf(wafCfg, reporter, trustedProxies));
    }

    // Auth
    JwtAuthConfig jwtCfg;
    RouteAuthConfig routeAuthCfg;
    if(root["auth"] && root["auth"]["jwt"]) {
        const auto& j = root["auth"]["jwt"];
        jwtCfg.enabled = j["enabled"] ? j["enabled"].as<bool>() : false;
        jwtCfg.algo    = norm_jwt_algo(j["algo"] ? j["algo"].as<std::string>() : "HS256");
        jwtCfg.secret  = j["secret"]  ? j["secret"].as<std::string>() : "";
        jwtCfg.pubKey  = j["public_key"] ? j["public_key"].as<std::string>() : "";
        jwtCfg.issuer  = j["issuer"]  ? j["issuer"].as<std::string>() : "";
        jwtCfg.audience = j["audience"] ? j["audience"].as<std::string>() : "";
        jwtCfg.leewaySec = j["leeway_sec"] ? j["leeway_sec"].as<uint32_t>() : 0;
        if(jwtCfg.algo.empty()) {
            BRONX_LOG_WARN(g_logger) << "auth.jwt.algo unsupported, jwt will reject";
        }
        if(j["keys"] && j["keys"].IsSequence()) {
            std::unordered_set<std::string> kids;
            for(const auto& item : j["keys"]) {
                if(!item.IsMap()) continue;
                JwtKey key;
                key.kid = item["kid"] ? item["kid"].as<std::string>() : "";
                key.algo = item["algo"] ? norm_jwt_algo(item["algo"].as<std::string>()) : jwtCfg.algo;
                key.pubKey = item["public_key"] ? item["public_key"].as<std::string>() : "";
                if(key.kid.empty() || key.pubKey.empty() || key.algo != jwtCfg.algo
                   || !kids.insert(key.kid).second) {
                    BRONX_LOG_WARN(g_logger) << "auth.jwt.keys bad entry, skipped";
                    continue;
                }
                jwtCfg.keys.push_back(std::move(key));
            }
            if(!jwtCfg.keys.empty() && !jwtCfg.pubKey.empty()) {
                BRONX_LOG_WARN(g_logger) << "auth.jwt.keys cannot mix with public_key";
            }
        }
        routeAuthCfg.globalJwtEnabled = jwtCfg.enabled;
    }
    if(root["auth"] && root["auth"]["api_key"]) {
        const auto& ak = root["auth"]["api_key"];
        if(ak["header"]) routeAuthCfg.apiKeyHeader = ak["header"].as<std::string>();
        if(ak["keys"] && ak["keys"].IsSequence()) {
            bool legacy = false;
            std::unordered_set<std::string> ids;
            for(const auto& k : ak["keys"]) {
                if(k.IsScalar()) {
                    routeAuthCfg.apiKeys.push_back(k.as<std::string>());
                    legacy = true;
                    continue;
                }
                if(k["key"]) {
                    routeAuthCfg.apiKeys.push_back(k["key"].as<std::string>());
                    legacy = true;
                    continue;
                }
                ApiKey key;
                key.id = k["id"] ? k["id"].as<std::string>() : "";
                key.hash = k["hash"] ? lower_ascii(k["hash"].as<std::string>()) : "";
                key.enabled = k["enabled"] ? k["enabled"].as<bool>() : true;
                if(!read_needs(k["scopes"], key.scopes) || key.id.empty()
                   || key.hash.size() != 64
                   || !ids.insert(key.id).second) {
                    BRONX_LOG_WARN(g_logger) << "auth.api_key.keys bad entry, skipped";
                    continue;
                }
                routeAuthCfg.keys.push_back(std::move(key));
            }
            if(legacy) {
                BRONX_LOG_WARN(g_logger) << "auth.api_key.keys plaintext is deprecated";
            }
        }
    }
    if(root["auth"] && root["auth"]["forward"]) {
        const auto& fw = root["auth"]["forward"];
        routeAuthCfg.fwdUser = fw["enabled"] ? fw["enabled"].as<bool>() : false;
        routeAuthCfg.stripBearer = fw["strip_bearer"]
            ? fw["strip_bearer"].as<bool>() : true;
        routeAuthCfg.stripApiKey = fw["strip_api_key"]
            ? fw["strip_api_key"].as<bool>() : true;
        if(fw["prefix"]) routeAuthCfg.fwdPrefix = fw["prefix"].as<std::string>();
    }
    auto jwtAuth = jwtCfg.enabled ? std::make_shared<JwtAuthenticator>(jwtCfg) : nullptr;

    // 路由匹配必须在 per-route 限流之前：限流器读 ctx.route().rule 的限流配置，
    // 路由未匹配时 rule 为空，限流会被跳过（曾导致 per-route 限流完全失效）。
    chain->use(MakeRouterMiddleware(snap->router, snap->upstreams));
    chain->use(MakeRouteAuthMiddleware(jwtAuth, routeAuthCfg));

    // Per-route RateLimit（在 Router 之后，此时 ctx.route().rule 已填充）
    // 传可信代理名单, 限流按真实客户端 IP 计数, 和名单用同一套解析
    // reporter+rateBanCfg: 同 IP 反复超限攒够阈值就报坏 IP 给 daemon
    chain->use(MakePerRouteRateLimitMiddleware(trustedProxies, reporter, rateBanCfg));

    // WebSocket 透明隧道必须在 Router/Auth/RateLimit 之后、普通 HTTP Proxy 之前：
    // 它需要 ctx.route().upstream 建立上游连接,命中 Upgrade 后终结链路并进入双泵透传。
    chain->use(MakeWebSocketTunnelStub());

    // Proxy（末端）
    chain->use(MakeProxyMiddleware(3000, 30000, trustedProxies));

    snap->chain = chain;

    // iom 为 CpuPool 线程上 offload 构建时由调用方传入的目标 reactor;为空则退回
    // Current()(启动路径在 reactor 协程上直接调时可取到)。post/addTimer 跨线程安全。
    if(!iom) {
        iom = bronx::BxIoManager::Current();
    }
    if(iom) {
        for(const auto& kv : snap->upstreams->all()) {
            auto group = kv.second;
            const auto& hc = group->healthCheck();
            if(!hc.enabled) continue;
            uint32_t interval = hc.intervalMs ? hc.intervalMs : 5000;
            iom->post([group]() {
                group->runHealthCheckOnce();
            });
            snap->healthTimers.push_back(iom->addTimer(interval, [group]() {
                group->runHealthCheckOnce();
            }, true));
        }
    }
    if(hasPolicy) {
        if(pending) *pending = std::move(policy);
        else guard->applyStatic(std::move(policy));
    }
    return snap;
}

ConfigSnapshot::ptr GatewayConfig::BuildSnapshotFromYaml(const YAML::Node& root,
                                                        bronx::BxIoManager* iom,
                                                        std::shared_ptr<bronx::ipban::Guard> guard,
                                                        std::shared_ptr<bronx::ipban::Reporter> reporter) {
    return BuildSnapshotFromYaml(root, iom, std::move(guard), std::move(reporter), nullptr);
}

ConfigSnapshot::ptr GatewayConfig::BuildSnapshot(const std::string& path,
                                                bronx::BxIoManager* iom,
                                                std::shared_ptr<bronx::ipban::Guard> guard,
                                                std::shared_ptr<bronx::ipban::Reporter> reporter) {
    YAML::Node root = YAML::LoadFile(path); // 失败抛 YAML::Exception
    return BuildSnapshotFromYaml(root, iom, guard, reporter);
}

} // namespace gateway
} // namespace bronx
