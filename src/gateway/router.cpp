#include "router.h"
#include "arc_cache.h"
#include "ups_group.h"
#include <algorithm>
#include <mutex>
#include <strings.h>
#include <cctype>

namespace bronx {
namespace gateway {

// RouteTable

static bool method_match(const std::vector<std::string>& allowed, const std::string& method) {
    if(allowed.empty()) return true;
    for(const auto& m : allowed)
        if(strcasecmp(m.c_str(), method.c_str()) == 0) return true;
    return false;
}

static std::string norm_host(std::string host) {
    // Host 匹配必须稳定：域名大小写无关，常见 "host:port" 形式要去端口；
    // IPv6 字面量含多个 ':'，不能按最后一个冒号盲切。
    auto first = host.find_first_not_of(" \t");
    auto last = host.find_last_not_of(" \t");
    if(first == std::string::npos) return "";
    host = host.substr(first, last - first + 1);

    if(!host.empty() && host[0] == '[') {
        auto end = host.find(']');
        if(end != std::string::npos) {
            host = host.substr(1, end - 1);
        }
    } else {
        auto colon = host.rfind(':');
        if(colon != std::string::npos
                && host.find(':') == colon) {
            host = host.substr(0, colon);
        }
    }
    std::transform(host.begin(), host.end(), host.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return host;
}

static std::string req_method_name(const GwRequest::ptr& req) {
    if(!req) return "";
    return req->getMethodRaw().empty()
        ? HttpMethodToString(req->getMethod()) : req->getMethodRaw();
}

static bool host_match(const std::string& ruleHost, const std::string& reqHost) {
    if(ruleHost.empty()) return true;
    return norm_host(ruleHost) == reqHost;
}

// 路径段前缀匹配：/api 匹配 /api/foo 但不匹配 /apifoo
static bool is_seg_prefix(const std::string& path, const std::string& prefix) {
    if(prefix == "/") return !path.empty() && path[0] == '/';
    if(path.size() < prefix.size()) return false;
    if(path.compare(0, prefix.size(), prefix) != 0) return false;
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}

bool RouteTable::ruleMatches(const RouteRule& r, const std::string& method,
                              const std::string& host, const std::string& path) const {
    if(!host_match(r.host, host)) return false;
    if(!method_match(r.methods, method)) return false;
    if(r.matchType == MatchType::EXACT) return path == r.pathPattern;
    return is_seg_prefix(path, r.pathPattern);
}

// 返回 <0 表示 a 优先于 b（a 的优先级更高）
int RouteTable::compareRules(const RouteRule& a, const RouteRule& b) const {
    // host 非空 > host 为空
    int ha = a.host.empty() ? 0 : 1, hb = b.host.empty() ? 0 : 1;
    if(ha != hb) return hb - ha;
    // methods 非空 > methods 为空
    int ma = a.methods.empty() ? 0 : 1, mb = b.methods.empty() ? 0 : 1;
    if(ma != mb) return mb - ma;
    // priority 降序
    if(a.priority != b.priority) return b.priority - a.priority;
    // 插入顺序升序（稳定）
    return (int)a.insertionIdx - (int)b.insertionIdx;
}

void RouteTable::addRule(RouteRule rule) {
    rule.insertionIdx = (uint32_t)m_rules.size();
    auto sp = std::make_shared<RouteRule>(std::move(rule));
    size_t idx = m_rules.size();
    m_rules.push_back(sp);
    if(sp->matchType == MatchType::EXACT)
        m_exactMap[sp->pathPattern].push_back(idx);
    else
        m_prefixVec.push_back(idx);
}

void RouteTable::build() {
    // 对 exactMap 每条 path 的规则列表排序
    for(auto& kv : m_exactMap) {
        std::stable_sort(kv.second.begin(), kv.second.end(),
            [this](size_t a, size_t b) {
                return compareRules(*m_rules[a], *m_rules[b]) < 0;
            });
    }
    // prefixVec：最长前缀优先；同长度按 tie-break
    std::stable_sort(m_prefixVec.begin(), m_prefixVec.end(),
        [this](size_t a, size_t b) {
            const auto& ra = *m_rules[a], &rb = *m_rules[b];
            if(ra.pathPattern.size() != rb.pathPattern.size())
                return ra.pathPattern.size() > rb.pathPattern.size();
            return compareRules(ra, rb) < 0;
        });
}

std::shared_ptr<RouteRule> RouteTable::match(const std::string& method,
                                              const std::string& host,
                                              const std::string& path) const {
    // EXACT 优先
    auto it = m_exactMap.find(path);
    if(it != m_exactMap.end()) {
        for(size_t idx : it->second) {
            if(ruleMatches(*m_rules[idx], method, host, path))
                return m_rules[idx];
        }
    }
    // PREFIX：prefixVec 已按最长前缀+tie-break 排序
    for(size_t idx : m_prefixVec) {
        if(ruleMatches(*m_rules[idx], method, host, path))
            return m_rules[idx];
    }
    return nullptr;
}

std::vector<RouteRule> RouteTable::listRules() const {
    std::vector<RouteRule> out;
    out.reserve(m_rules.size());
    for(const auto& r : m_rules) {
        out.push_back(*r);
    }
    return out;
}

// Router

Router::Router() : m_cache(std::make_shared<ArcRouteCache>()) {
    m_key_extractor = [](const ReqCtx& ctx) -> std::string {
        auto req = ctx.request();
        return req ? req->getPath() : std::string();
    };
}

void Router::setTable(std::shared_ptr<RouteTable> table) {
    std::lock_guard lk(m_pending_mtx);
    m_table = std::move(table);
    m_pending = m_table ? m_table->listRules() : std::vector<RouteRule>{};
    m_gen.fetch_add(1, std::memory_order_relaxed);
    if(m_cache) m_cache->invalidate();
}

size_t Router::routeCount() const {
    std::lock_guard lk(m_pending_mtx);
    return m_table ? m_table->ruleCount() : 0;
}

std::vector<RouteRule> Router::listRoutes() const {
    std::lock_guard lk(m_pending_mtx);
    return m_table ? m_table->listRules() : std::vector<RouteRule>{};
}

void Router::addRoute(RouteRule rule) {
    std::lock_guard lk(m_pending_mtx);
    m_pending.push_back(std::move(rule));
    auto table = std::make_shared<RouteTable>();
    for(auto& r : m_pending) table->addRule(r);
    table->build();
    m_table = std::move(table);
    m_gen.fetch_add(1, std::memory_order_relaxed);
    if(m_cache) m_cache->invalidate();
}

void Router::clearRoutes() {
    std::lock_guard lk(m_pending_mtx);
    m_pending.clear();
    m_table = std::make_shared<RouteTable>();
    m_gen.fetch_add(1, std::memory_order_relaxed);
    if(m_cache) m_cache->invalidate();
}

void Router::setKeyExtractor(KeyExtractor ke) {
    std::lock_guard lk(m_pending_mtx);
    m_key_extractor = std::move(ke);
    m_gen.fetch_add(1, std::memory_order_relaxed);
    if(m_cache) m_cache->invalidate();
}

bool Router::match(ReqCtx& ctx, const UpstreamRegistry& upstreams) const {
    auto req = ctx.request();
    if(!req) { ctx.route().matched = false; return false; }

    std::string method = req_method_name(req);
    std::string host   = norm_host(req->getHeader("Host"));
    std::shared_ptr<RouteTable> table;
    RouteCache::ptr cache;
    KeyExtractor keyExtractor;
    uint32_t gen = 0;
    {
        std::lock_guard lk(m_pending_mtx);
        table = m_table;
        cache = m_cache;
        keyExtractor = m_key_extractor;
        gen = m_gen.load(std::memory_order_relaxed);
    }
    std::string path = keyExtractor ? keyExtractor(ctx) : req->getPath();

    std::string key = std::to_string(gen) + "|" + method + "|" + host + "|" + path;

    // 先查 cache
    RouteResult cached;
    if(cache && cache->get(key, cached)) {
        ctx.route() = cached;
        return cached.matched;
    }

    RouteResult result;
    if(table) {
        auto rule = table->match(method, host, path);
        if(rule) {
            result.matched   = true;
            result.rule      = rule;
            result.routeKey  = rule->pathPattern;
            result.upstream  = upstreams.get(rule->upstream);
        }
    }
    if(cache) cache->put(key, result);
    ctx.route() = result;
    return result.matched;
}

} // namespace gateway
} // namespace bronx
