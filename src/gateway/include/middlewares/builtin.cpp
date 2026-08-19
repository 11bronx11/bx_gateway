#include "middlewares/builtin.h"
#include "str.h"
#include "conn.h"
#include "http_msg.h"
#include "ctx.h"
#include "router.h"
#include "ups_group.h"
#include "metrics.h"
#include "util.h"
#include "log.h"
#include "report.h"
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <arpa/inet.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <limits>
#include <strings.h>
#include <openssl/sha.h>
namespace bronx {
namespace gateway {

static bronx::BxLogger::ptr g_gwlog = BRONX_LOG_NAME("system");

static bool is_cors_preflight(const GwRequest::ptr& req) {
    return req
        && req->getMethod() == HttpMethod::OPTIONS
        && !trim_ascii(req->getHeader("Origin")).empty()
        && !trim_ascii(req->getHeader("Access-Control-Request-Method")).empty();
}

static std::string pick_origin(const CorsOptions& opts, const std::string& origin) {
    std::string reqOrigin = trim_ascii(origin);
    if(reqOrigin.empty()) {
        return "";
    }
    for(const auto& allowed : opts.allowOrigins) {
        std::string v = trim_ascii(allowed);
        if(v == "*") {
            return opts.allowCredentials ? reqOrigin : "*";
        }
        if(strcasecmp(v.c_str(), reqOrigin.c_str()) == 0) {
            return reqOrigin;
        }
    }
    if(!opts.allowOrigins.empty()) {
        return "";
    }
    std::string single = trim_ascii(opts.allowOrigin);
    if(single.empty()) {
        return "";
    }
    if(single == "*") {
        return opts.allowCredentials ? reqOrigin : "*";
    }
    if(strcasecmp(single.c_str(), reqOrigin.c_str()) == 0) {
        return reqOrigin;
    }
    return "";
}

// 返回 origin 是否过了白名单(过了才贴头),让调用方据此记 denied
static bool add_cors_hdrs(ReqCtx& ctx, const CorsOptions& opts, bool preflight) {
    if(!opts.enabled) {
        return false;
    }
    auto req = ctx.request();
    if(!req) {
        return false;
    }
    std::string origin = trim_ascii(req->getHeader("Origin"));
    std::string allowedOrigin = pick_origin(opts, origin);
    if(allowedOrigin.empty()) {
        return false;
    }
    ctx.add_resp_hdr("Access-Control-Allow-Origin", allowedOrigin);
    if(allowedOrigin != "*") {
        ctx.add_resp_hdr("Vary", "Origin");
    }
    if(opts.allowCredentials) {
        ctx.add_resp_hdr("Access-Control-Allow-Credentials", "true");
    }
    if(preflight) {
        ctx.add_resp_hdr("Access-Control-Allow-Methods", opts.allowMethods);
        std::string reqHeaders = trim_ascii(req->getHeader("Access-Control-Request-Headers"));
        ctx.add_resp_hdr("Access-Control-Allow-Headers",
                              reqHeaders.empty() ? opts.allowHeaders : reqHeaders);
        ctx.add_resp_hdr("Access-Control-Max-Age", std::to_string(opts.maxAge));
    }
    return true;
}

static std::string resolved_client_ip(ReqCtx& ctx, const std::vector<bronx::ipban::Ip>& trusted) {
    return loadClientAddr(ctx, trusted) ? ctx.clientAddr().client.toString() : "";
}

static std::string peer_client_ip(ReqCtx& ctx) {
    return ctx.connection() ? ctx.connection()->peerAddr() : "";
}

static std::string json_esc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for(unsigned char c : s) {
        switch(c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if(c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0x0f]);
                    out.push_back(hex[c & 0x0f]);
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    return out;
}

Middleware::ptr MakeCorsPrepareMiddleware(const CorsOptions& opts) {
    return std::make_shared<FuncMiddleware>(
        [opts](ReqCtx& ctx, const NextFn& next) {
            if(!opts.enabled) {
                ctx.setAttr("cors_preflight", false);
                ctx.setAttr("cors_short", false);
                next();
                return;
            }
            bool preflight = is_cors_preflight(ctx.request());
            bool allowed = add_cors_hdrs(ctx, opts, preflight);
            // 带了 origin 却没过白名单,记一笔软拒绝
            auto req = ctx.request();
            std::string origin = req ? trim_ascii(req->getHeader("Origin")) : "";
            if(!origin.empty() && !allowed) {
                GatewayMetrics::instance().incr_cors_denied();
                BRONX_LOG_WARN(g_gwlog) << "cors origin denied: " << origin;
            }
            ctx.setAttr("cors_preflight", preflight);
            ctx.setAttr("cors_short", opts.shortCircuitPreflight);
            next();
        }, "cors_prepare");
}

Middleware::ptr MakeCorsPreflightMiddleware() {
    return std::make_shared<FuncMiddleware>(
        [](ReqCtx& ctx, const NextFn& next) {
            bool preflight = false;
            bool shortCircuit = false;
            ctx.getAttr("cors_preflight", preflight);
            ctx.getAttr("cors_short", shortCircuit);
            if(preflight && shortCircuit) {
                GatewayMetrics::instance().incr_cors_preflight();
                reply(ctx, 204, "");
                return;
            }
            next();
        }, "cors_preflight");
}

Middleware::ptr MakeCorsMiddleware(const CorsOptions& opts) {
    auto prepare = MakeCorsPrepareMiddleware(opts);
    auto preflight = MakeCorsPreflightMiddleware();
    return std::make_shared<FuncMiddleware>(
        [prepare, preflight](ReqCtx& ctx, const NextFn& next) {
            prepare->handle(ctx, [&]() {
                preflight->handle(ctx, next);
            });
        }, "cors");
}

// 重定向
Middleware::ptr MakeRedirectMiddleware(const std::vector<RedirectRule>& rules) {
    return std::make_shared<FuncMiddleware>(
        [rules](ReqCtx& ctx, const NextFn& next) {
            auto req = ctx.request();
            if(req) {
                const std::string& path = req->getPath();
                for(const auto& r : rules) {
                    if(path.compare(0, r.fromPrefix.size(), r.fromPrefix) == 0) {
                        HeaderMap h;
                        h["Location"] = r.toLocation;
                        reply(ctx, r.status, "", h);
                        return;
                    }
                }
            }
            next();
        }, "redirect");
}

// 健康检查
Middleware::ptr MakeHealthCheckMiddleware(const std::string& path) {
    return std::make_shared<FuncMiddleware>(
        [path](ReqCtx& ctx, const NextFn& next) {
            auto req = ctx.request();
            if(req && req->getPath() == path) {
                reply(ctx, 200, "OK\n");
                return;
            }
            next();
        }, "health");
}

// <!-- PLACEHOLDER_BUILTIN2 -->

// 安全响应头:登记到 ctx 的追加响应头(代理回程合并;短路响应也可手动加)
Middleware::ptr MakeSecurityHeadersMiddleware(const SecurityHeadersOptions& opts) {
    return std::make_shared<FuncMiddleware>(
        [opts](ReqCtx& ctx, const NextFn& next) {
            if(!opts.contentSecurityPolicy.empty())
                ctx.add_resp_hdr("Content-Security-Policy", opts.contentSecurityPolicy);
            if(!opts.strictTransportSecurity.empty())
                ctx.add_resp_hdr("Strict-Transport-Security", opts.strictTransportSecurity);
            if(!opts.frameOptions.empty())
                ctx.add_resp_hdr("X-Frame-Options", opts.frameOptions);
            if(!opts.contentTypeOptions.empty())
                ctx.add_resp_hdr("X-Content-Type-Options", opts.contentTypeOptions);
            if(!opts.referrerPolicy.empty())
                ctx.add_resp_hdr("Referrer-Policy", opts.referrerPolicy);
            next();
        }, "security_headers");
}

// 维护模式:开着就全站短路 503,关着直接放行
Middleware::ptr MakeMaintenanceMiddleware(const MaintOptions& opts) {
    return std::make_shared<FuncMiddleware>(
        [opts](ReqCtx& ctx, const NextFn& next) {
            if(!MaintGate::instance().on()) {
                next();
                return;
            }
            HeaderMap h;
            h["Retry-After"] = std::to_string(opts.retryAfterSec);
            reply(ctx, 503, opts.body, h);
            GatewayMetrics::instance().incr_maint_blocked();
        }, "maintenance");
}

// request-id:请求无该头则生成,注入【请求头】(透传上游)+ 登记到响应头(回客户端)
Middleware::ptr MakeRequestIdMiddleware(const std::string& headerName) {
    return std::make_shared<FuncMiddleware>(
        [headerName](ReqCtx& ctx, const NextFn& next) {
            auto req = ctx.request();
            std::string id = req ? req->getHeader(headerName) : "";
            if(id.empty()) {
                // 生成 16 位十六进制随机 id(自包含,不依赖外部 random_string)
                static const char* hex = "0123456789abcdef";
                uint64_t t = bronx::GetCurrentMs();
                static std::atomic<uint64_t> seq{0};
                uint64_t mix = t ^ (seq.fetch_add(1) << 20) ^ (uint64_t)(uintptr_t)&id;
                id.resize(16);
                for(int i = 0; i < 16; ++i) { id[i] = hex[mix & 0xF]; mix = (mix >> 4) | (mix << 60); }
                if(req) req->setHeader(headerName, id);
            }
            ctx.add_resp_hdr(headerName, id);
            ctx.setAttr("request_id", id);   // 存进 ctx,access log 从这读
            next();
        }, "request_id");
}

Middleware::ptr MakeClientAddrMiddleware(std::vector<bronx::ipban::Ip> trustedProxies) {
    return std::make_shared<FuncMiddleware>(
        [trusted = std::move(trustedProxies)](ReqCtx& ctx, const NextFn& next) {
            loadClientAddr(ctx, trusted);
            next();
        }, "client_addr");
}

Middleware::ptr MakeFinishMiddleware() {
    return std::make_shared<FuncMiddleware>(
        [](ReqCtx& ctx, const NextFn& next) {
            next();
            if(ctx.respState() == RespState::OPEN) {
                GatewayMetrics::instance().incrFinish500();
                reply(ctx, 500, std::string(HttpStatusReason(500)) + "\n", {}, true);
            }
        }, "finish");
}

// 访问日志:回程记录 方法/路径/状态
Middleware::ptr MakeAccessLogMiddleware() {
    return std::make_shared<FuncMiddleware>(
        [](ReqCtx& ctx, const NextFn& next) {
            next();   // 先处理
            if(g_gwlog->getLevel() > BxLogLevel::INFO) return;
            auto req = ctx.request();
            int status = ctx.response() ? ctx.response()->getStatus() : 0;
            std::string auth;
            std::string authResult;
            ctx.getAttr("auth", auth);
            ctx.getAttr("auth_result", authResult);
            BRONX_LOG_INFO(g_gwlog) << "[access] "
                << (req ? HttpMethodToString(req->getMethod()) : "?")
                << " " << (req ? req->getPath() : "?")
                << " -> " << status << " auth=" << auth
                << " auth_result=" << authResult;
        }, "access_log");
}

// 令牌桶
TokenBucket::TokenBucket(double capacity, double refillPerSec)
    : m_capacity((std::isfinite(capacity) && capacity > 0.0) ? capacity : 0.0)
    , m_refill_per_sec((std::isfinite(refillPerSec) && refillPerSec > 0.0) ? refillPerSec : 0.0)
    , m_tokens(m_capacity), m_lastMs(bronx::GetCurrentMs()) {}

bool TokenBucket::tryAcquire(double n) {
    return tryAcquire(n, nullptr);
}

void TokenBucket::refill_locked(uint64_t now) {
    if(now > m_lastMs && m_refill_per_sec > 0.0) {
        double elapsed = (now - m_lastMs) / 1000.0;
        m_tokens += elapsed * m_refill_per_sec;
        if(m_tokens > m_capacity) m_tokens = m_capacity;
    }
    m_lastMs = now;
}

uint32_t TokenBucket::retry_after_locked(double n) const {
    if(m_tokens >= n) return 0;
    if(m_capacity < n || m_refill_per_sec <= 0.0) return 1;
    double wait = (n - m_tokens) / m_refill_per_sec;
    if(wait <= 0.0) return 1;
    double maxSec = (double)std::numeric_limits<uint32_t>::max();
    if(wait >= maxSec) return std::numeric_limits<uint32_t>::max();
    uint32_t sec = (uint32_t)std::ceil(wait);
    return sec == 0 ? 1 : sec;
}

bool TokenBucket::tryAcquire(double n, uint32_t* retryAfterSec) {
    if(n <= 0.0) {
        if(retryAfterSec) *retryAfterSec = 0;
        return true;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    uint64_t now = bronx::GetCurrentMs();
    // 按流逝时间补充令牌,不超过容量;系统时间回退时不做无符号下溢补偿。
    refill_locked(now);
    if(m_tokens >= n) {
        m_tokens -= n;
        if(retryAfterSec) *retryAfterSec = 0;
        return true;
    }
    if(retryAfterSec) *retryAfterSec = retry_after_locked(n);
    return false;
}

// 限流中间件:全局单桶,按请求数计(可换成 token 数)。超限 429 短路
Middleware::ptr MakeRateLimitMiddleware(double capacity, double refillPerSec) {
    auto bucket = std::make_shared<TokenBucket>(capacity, refillPerSec);
    return std::make_shared<FuncMiddleware>(
        [bucket](ReqCtx& ctx, const NextFn& next) {
            uint32_t retryAfter = 1;
            if(!bucket->tryAcquire(1.0, &retryAfter)) {
                HeaderMap h;
                h["Retry-After"] = std::to_string(retryAfter ? retryAfter : 1);
                reply(ctx, 429, "Too Many Requests\n", h);
                GatewayMetrics::instance().incrRateLimited();
                return;
            }
            next();
        }, "rate_limit");
}

Middleware::ptr MakeStructuredAccessLogMiddleware(std::vector<bronx::ipban::Ip> trustedProxies) {
    return std::make_shared<FuncMiddleware>(
        [trusted = std::move(trustedProxies)](ReqCtx& ctx, const NextFn& next) {
            next();
            uint64_t end = bronx::GetCurrentMs();
            uint64_t t0 = ctx.startMs();
            uint64_t duration = end >= t0 ? end - t0 : 0;
            uint64_t commit = ctx.commitMs();
            uint64_t latency = commit >= t0 ? commit - t0 : duration;
            auto rsp = ctx.response();
            const auto& route = ctx.route();
            int status = ctx.status();
            if(status == 0 && rsp) status = rsp->getStatus();
            // 延迟和路由指标
            auto& metrics = GatewayMetrics::instance();
            metrics.recordLatency(latency);
            if(route.matched) metrics.recordRoute(route.routeKey, status);
            if(g_gwlog->getLevel() > BxLogLevel::INFO) return;

            auto req = ctx.request();
            std::string ip = resolved_client_ip(ctx, trusted);
            std::string reqId;
            ctx.getAttr("request_id", reqId);   // request id 已存好, 直接写日志
            std::string auth;
            std::string authResult;
            ctx.getAttr("auth", auth);
            ctx.getAttr("auth_result", authResult);
            std::ostringstream ss;
            ss << "{\"ts\":"       << t0
               << ",\"request_id\":\"" << json_esc(reqId) << "\""
               << ",\"method\":\"" << json_esc(req ? HttpMethodToString(req->getMethod()) : "") << "\""
               << ",\"path\":\""   << json_esc(req ? req->getPath() : "") << "\""
               << ",\"status\":"   << status
               << ",\"latency_ms\":" << latency
               << ",\"duration_ms\":" << duration
               << ",\"ip\":\""     << json_esc(ip) << "\""
               << ",\"route\":\""  << json_esc(route.routeKey) << "\""
               << ",\"upstream\":\"" << json_esc(route.upstream ? route.upstream->name() : "") << "\""
               << ",\"auth\":\"" << json_esc(auth) << "\""
               << ",\"auth_result\":\"" << json_esc(authResult) << "\""
               << "}";
            BRONX_LOG_INFO(g_gwlog) << ss.str();
        }, "access_log_json");
}

// M7 Per-route RateLimit
Middleware::ptr MakePerRouteRateLimitMiddleware(std::vector<bronx::ipban::Ip> trustedProxies,
                                                std::shared_ptr<bronx::ipban::Reporter> reporter,
                                                RateBanCfg banCfg,
                                                size_t bucketMax) {
    static constexpr uint64_t kBucketIdleTtlMs = 10 * 60 * 1000;
    static constexpr uint64_t kBucketCleanupIntervalMs = 60 * 1000;
    struct State {
        struct Entry {
            std::shared_ptr<TokenBucket> bucket;
            uint64_t lastSeenMs = 0;
            double capacity = 0.0;
            double refillPerSec = 0.0;
        };
        // 一个 IP 在当前窗口攒了几次 429
        struct Hit {
            uint32_t n = 0;
            uint64_t winStartMs = 0;
        };

        std::mutex mtx;
        std::unordered_map<std::string, Entry> buckets;
        std::unordered_map<std::string, Hit> hits;   // key 是 IP 字符串
        std::shared_ptr<TokenBucket> full = std::make_shared<TokenBucket>(0.0, 0.0);
        size_t bucketMax = 1;
        uint64_t nextCleanupMs = 0;

        explicit State(size_t max) : bucketMax(max ? max : 1) {}

        void cleanupLocked(uint64_t now, uint64_t windowMs) {
            for(auto it = buckets.begin(); it != buckets.end();) {
                if(now >= it->second.lastSeenMs
                   && now - it->second.lastSeenMs > kBucketIdleTtlMs) {
                    it = buckets.erase(it);
                } else {
                    ++it;
                }
            }
            // hits 清理窗口改为 3倍窗口期(最少5分钟),防止内存泄漏
            uint64_t hitsTtl = std::max(windowMs * 3, 5 * 60 * 1000UL);
            for(auto it = hits.begin(); it != hits.end();) {
                if(now >= it->second.winStartMs
                   && now - it->second.winStartMs > hitsTtl) {
                    it = hits.erase(it);
                } else {
                    ++it;
                }
            }
            // 防御：hits map 超过1万条强制淘汰最老的20%
            if(hits.size() > 10000) {
                std::vector<std::pair<uint64_t, std::string>> ages;
                ages.reserve(hits.size());
                for(const auto& kv : hits) {
                    ages.push_back({kv.second.winStartMs, kv.first});
                }
                std::sort(ages.begin(), ages.end());
                size_t toRemove = hits.size() / 5;
                for(size_t i = 0; i < toRemove && i < ages.size(); ++i) {
                    hits.erase(ages[i].second);
                }
            }
            nextCleanupMs = now + kBucketCleanupIntervalMs;
        }

        std::shared_ptr<TokenBucket> get(const std::string& k, double cap, double refill, uint64_t windowMs) {
            uint64_t now = bronx::GetCurrentMs();
            std::lock_guard<std::mutex> lk(mtx);
            if(nextCleanupMs == 0 || now >= nextCleanupMs) {
                cleanupLocked(now, windowMs);
            }
            auto it = buckets.find(k);
            if(it != buckets.end()) {
                it->second.lastSeenMs = now;
                if(it->second.capacity == cap && it->second.refillPerSec == refill) {
                    return it->second.bucket;
                }
            }
            if(buckets.size() >= bucketMax) return full;
            auto b = std::make_shared<TokenBucket>(cap, refill);
            buckets[k] = Entry{b, now, cap, refill};
            return b;
        }

        // 记一次 429, 攒够阈值返回本窗口起点(拿去当 risk.id 去重), 否则返回 0。
        uint64_t bump429(const std::string& ip, uint32_t hitsMax, uint64_t windowMs) {
            uint64_t now = bronx::GetCurrentMs();
            std::lock_guard<std::mutex> lk(mtx);
            auto& h = hits[ip];
            // 防时钟回拨: now < winStartMs 时强制重置窗口
            if(h.winStartMs == 0 || now < h.winStartMs || now - h.winStartMs > windowMs) {
                h.winStartMs = now;
                h.n = 0;
            }
            ++h.n;
            if(h.n >= hitsMax) {
                uint64_t win = h.winStartMs;
                h.winStartMs = now;   // 开新窗口, 别一直刷
                h.n = 0;
                return win;
            }
            return 0;
        }
    };
    auto state = std::make_shared<State>(bucketMax);
    return std::make_shared<FuncMiddleware>(
        [state, trusted = std::move(trustedProxies), reporter, banCfg]
        (ReqCtx& ctx, const NextFn& next) {
            const auto& route = ctx.route();
            if(!route.matched || !route.rule || !route.rule->rateLimitEnabled) {
                next(); return;
            }
            const auto& rule = *route.rule;
            std::string keyVal;
            std::string keyType = "ip";
            if(rule.rateLimitKey == "user") {
                if(ctx.getAttr("jwt_sub", keyVal) && !keyVal.empty()) {
                    keyType = "user";
                } else {
                    keyVal = resolved_client_ip(ctx, trusted);
                }
            } else {
                // 按可信代理名单解析真实客户端 IP; 名单空时退回 TCP peer。
                keyVal = resolved_client_ip(ctx, trusted);
            }
            if(keyVal.empty()) keyVal = "unknown";
            std::string routeName = !rule.name.empty() ? rule.name : route.routeKey;
            if(routeName.empty()) routeName = rule.pathPattern;
            double cap = std::max(0.0, rule.rateLimitCapacity);
            double refill = std::max(0.0, rule.rateLimitRefillPerSec);
            auto bucket = state->get(routeName + "|" + keyType + ":" + keyVal,
                                     cap, refill, banCfg.windowMs);
            uint32_t retryAfter = 1;
            if(!bucket->tryAcquire(1.0, &retryAfter)) {
                HeaderMap h; h["Retry-After"] = std::to_string(retryAfter ? retryAfter : 1);
                reply(ctx, 429, "Too Many Requests\n", h);
                GatewayMetrics::instance().incrRateLimited();
                // 攒够阈值就报一条坏 IP 给 daemon。按真实客户端 IP 报, 即便限流 key 是 user。
                if(reporter && banCfg.on) {
                    std::string ipStr = resolved_client_ip(ctx, trusted);
                    bronx::ipban::Ip ip;
                    if(!ipStr.empty() && bronx::ipban::parseCidr(ipStr, ip)) {
                        uint64_t win = state->bump429(ipStr, banCfg.hits, banCfg.windowMs);
                        if(win) {
                            try {
                                bronx::ipban::Risk r;
                                r.id = ipStr + "@" + std::to_string(win);
                                r.ip = ip;
                                r.src = bronx::ipban::Src::RATE;
                                r.type = bronx::ipban::RiskType::RATE_ABUSE;
                                r.banMs = banCfg.banMs;
                                r.atMs = bronx::GetCurrentMs();
                                r.reason = "rate abuse route=" + routeName;
                                if(reporter->tryReport(r)) {
                                    GatewayMetrics::instance().incr_rate_reported();
                                }
                            } catch(const std::exception& e) {
                                BRONX_LOG_ERROR(g_gwlog) << "rate report failed: " << e.what();
                            }
                        }
                    }
                }
                return;
            }
            next();
        }, "per_route_rate_limit");
}

// M8 IP Filter
struct CidrRule {
    uint32_t network = 0;
    uint32_t mask = 0xffffffffu;
    bool valid = false;
};

static bool parseIPv4(const std::string& ip, uint32_t& out) {
    in_addr addr{};
    if(::inet_pton(AF_INET, ip.c_str(), &addr) != 1) return false;
    out = ntohl(addr.s_addr);
    return true;
}

static CidrRule parse_cidr(const std::string& raw) {
    CidrRule r;
    std::string ip = raw;
    uint32_t prefix = 32;
    auto slash = raw.find('/');
    if(slash != std::string::npos) {
        ip = raw.substr(0, slash);
        std::string p = raw.substr(slash + 1);
        if(p.empty()) return r;
        char* end = nullptr;
        long v = strtol(p.c_str(), &end, 10);
        if(end == p.c_str() || *end != '\0' || v < 0 || v > 32) return r;
        prefix = (uint32_t)v;
    }
    uint32_t addr = 0;
    if(!parseIPv4(ip, addr)) return r;
    r.mask = prefix == 0 ? 0 : (0xffffffffu << (32 - prefix));
    r.network = addr & r.mask;
    r.valid = true;
    return r;
}

static bool cidr_has(const std::vector<CidrRule>& rules, const std::string& ip) {
    uint32_t addr = 0;
    if(!parseIPv4(ip, addr)) return false;
    for(const auto& r : rules) {
        if(r.valid && ((addr & r.mask) == r.network)) return true;
    }
    return false;
}

Middleware::ptr MakeIPFilterMiddleware(const IPFilterConfig& cfg) {
    std::vector<CidrRule> rules;
    rules.reserve(cfg.cidrs.size());
    for(const auto& c : cfg.cidrs) {
        auto r = parse_cidr(c);
        if(r.valid) rules.push_back(r);
        else BRONX_LOG_WARN(g_gwlog) << "invalid ip_filter cidr ignored: " << c;
    }
    bool denylist = (cfg.mode != "allowlist");
    return std::make_shared<FuncMiddleware>(
        [cfg, rules, denylist](ReqCtx& ctx, const NextFn& next) {
            if(!cfg.enabled) { next(); return; }
            // 安全门控必须用 TCP peer 地址，绝不信任 X-Forwarded-For —— 否则被拉黑的
            // 客户端可伪造 XFF 绕过过滤。XFF 仅在前置代理可信时有意义（trusted proxy
            // 配置为 N+1 项）；当前实现一律以 peer 地址为准。
            std::string ip = peer_client_ip(ctx);
            bool inList = cidr_has(rules, ip);
            bool block  = denylist ? inList : !inList;
            if(block) { reply(ctx, 403, "Forbidden\n"); return; }
            next();
        }, "ip_filter");
}

static bool const_time_eq(const std::string& a, const std::string& b) {
    size_t maxLen = std::max(a.size(), b.size());
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    for(size_t i = 0; i < maxLen; ++i) {
        unsigned char ca = i < a.size() ? (unsigned char)a[i] : 0;
        unsigned char cb = i < b.size() ? (unsigned char)b[i] : 0;
        diff |= ca ^ cb;
    }
    return diff == 0;
}

static bool apikey_ok(const std::vector<std::string>& keys, const std::string& got) {
    if(got.empty()) return false;
    bool ok = false;
    for(const auto& k : keys) {
        ok = const_time_eq(k, got) || ok;
    }
    return ok;
}

static std::string key_hash(const std::string& s) {
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)s.data(), s.size(), out);
    static const char hex[] = "0123456789abcdef";
    std::string val;
    val.reserve(SHA256_DIGEST_LENGTH * 2);
    for(unsigned char c : out) {
        val.push_back(hex[c >> 4]);
        val.push_back(hex[c & 15]);
    }
    return val;
}

static const ApiKey* apikey_find(const std::vector<ApiKey>& keys,
                                 const std::string& got) {
    size_t dot = got.find('.');
    if(dot == std::string::npos || dot == 0 || dot + 1 >= got.size()) return nullptr;
    std::string id = got.substr(0, dot);
    std::string hash = key_hash(got.substr(dot + 1));
    const ApiKey* found = nullptr;
    for(const auto& key : keys) {
        if(key.id != id || !key.enabled || key.hash.size() != hash.size()) continue;
        if(const_time_eq(key.hash, hash) && !found) found = &key;
    }
    return found;
}

static size_t auth_result(AuthErr e) {
    switch(e) {
        case AuthErr::Ok: return 0;
        case AuthErr::NoToken: return 1;
        case AuthErr::BadToken: return 2;
        case AuthErr::BadKey: return 3;
        case AuthErr::BadSig: return 4;
        case AuthErr::Expired: return 5;
        case AuthErr::BadIss: return 6;
        case AuthErr::BadAud: return 7;
        case AuthErr::BadAlg: return 8;
        case AuthErr::Scope: return 9;
        case AuthErr::Role: return 10;
        case AuthErr::BadPolicy: return 11;
    }
    return 11;
}

static const char* auth_name(AuthErr e) {
    static const char* names[] = {
        "ok", "missing", "bad_token", "bad_key", "bad_sig", "expired",
        "bad_iss", "bad_aud", "bad_alg", "scope", "role", "bad_policy"
    };
    return names[auth_result(e)];
}

static void old_auth(AuthErr e) {
    auto& m = GatewayMetrics::instance();
    switch(e) {
        case AuthErr::Ok:      m.incr_auth_ok(); break;
        case AuthErr::NoToken: m.incr_auth_no_token(); break;
        case AuthErr::Expired: m.incr_auth_expired(); break;
        default:               m.incr_auth_bad_sig(); break;   // 签名/算法/iss/aud 都归这
    }
}

static void mark_auth(ReqCtx& ctx, const char* scheme, size_t schemeId,
                      AuthErr e, const std::string& route) {
    GatewayMetrics::instance().note_auth(schemeId, auth_result(e), route);
    ctx.setAttr("auth", std::string(scheme));
    ctx.setAttr("auth_result", std::string(auth_name(e)));
}

// 授权: scope 全都要有, roles 命中任一即可, 两边都过才 true。
static AuthErr authz_err(const RouteRule& rule, const ReqCtx& ctx) {
    if(!rule.reqScopes.empty()) {
        std::vector<std::string> have;
        if(!ctx.getAttr("auth_scopes", have)) ctx.getAttr("jwt_scopes", have);
        std::unordered_set<std::string> hs(have.begin(), have.end());
        for(const auto& need : rule.reqScopes)
            if(hs.find(need) == hs.end()) return AuthErr::Scope;
    }
    if(!rule.reqRoles.empty()) {
        std::vector<std::string> have;
        ctx.getAttr("jwt_roles", have);
        std::unordered_set<std::string> hs(have.begin(), have.end());
        bool hit = false;
        for(const auto& need : rule.reqRoles)
            if(hs.find(need) != hs.end()) { hit = true; break; }
        if(!hit) return AuthErr::Role;
    }
    return AuthErr::Ok;
}

// 逗号拼一下, 透传头用
static std::string join_comma(const std::vector<std::string>& v) {
    std::string s;
    for(size_t i = 0; i < v.size(); ++i) { if(i) s += ","; s += v[i]; }
    return s;
}

static bool good_user_val(const std::string& val) {
    if(val.empty()) return false;
    for(unsigned char c : val) {
        if(c <= 0x20 || c == 0x7f || c == ',') return false;
    }
    return true;
}

static std::string join_user_vals(const std::vector<std::string>& vals) {
    std::vector<std::string> good;
    for(const auto& val : vals) {
        if(good_user_val(val)) good.push_back(val);
    }
    return join_comma(good);
}

// 剥掉客户端自带的 X-User-* 头。任何路由只要开了透传都得先剥,
// 不然公开路由或 api_key 路由能把伪造身份头直接送到上游。
static void strip_user_hdrs(const RouteAuthConfig& cfg, ReqCtx& ctx) {
    if(!cfg.fwdUser) return;
    auto req = ctx.request();
    if(!req) return;
    req->delHeader(cfg.fwdPrefix + "Id");
    req->delHeader(cfg.fwdPrefix + "Scopes");
    req->delHeader(cfg.fwdPrefix + "Roles");
}

// 把验出来的身份注入上游请求。剥头已在入口做过,这里只管注入。
static void fwd_user(const RouteAuthConfig& cfg, ReqCtx& ctx) {
    if(!cfg.fwdUser) return;
    auto req = ctx.request();
    if(!req) return;
    std::string sub;
    if(ctx.getAttr("jwt_sub", sub) && good_user_val(sub)) req->setHeader(cfg.fwdPrefix + "Id", sub);
    std::vector<std::string> scopes, roles;
    if(ctx.getAttr("jwt_scopes", scopes)) {
        std::string val = join_user_vals(scopes);
        if(!val.empty()) req->setHeader(cfg.fwdPrefix + "Scopes", val);
    }
    if(ctx.getAttr("jwt_roles", roles)) {
        std::string val = join_user_vals(roles);
        if(!val.empty()) req->setHeader(cfg.fwdPrefix + "Roles", val);
    }
}

Middleware::ptr MakeRouteAuthMiddleware(Authenticator::ptr jwtAuth,
                                        const RouteAuthConfig& cfg) {
    return std::make_shared<FuncMiddleware>(
        [jwtAuth, cfg](ReqCtx& ctx, const NextFn& next) {
            // 先剥客户端伪造的身份头, 放行前不管走哪条 policy 都不能带进上游
            strip_user_hdrs(cfg, ctx);
            const auto& route = ctx.route();
            std::string policy = "none";
            if(route.rule) {
                policy = route.rule->authPolicy.empty() ? "inherit" : route.rule->authPolicy;
            }
            std::string routeName = route.rule && !route.rule->name.empty()
                ? route.rule->name : "none";
            if(policy == "inherit") {
                policy = cfg.globalJwtEnabled ? "jwt" : "none";
            }
            if(route.rule && ((!route.rule->reqScopes.empty()
                               && policy != "jwt" && policy != "api_key")
                              || (!route.rule->reqRoles.empty() && policy != "jwt"))) {
                GatewayMetrics::instance().incr_auth_forbidden();
                mark_auth(ctx, policy == "api_key" ? "api_key" : "other",
                          policy == "api_key" ? 1 : 2, AuthErr::BadPolicy, routeName);
                reply(ctx, 403, "Forbidden\n");
                return;
            }
            if(policy == "none") {
                mark_auth(ctx, "other", 2, AuthErr::Ok, routeName);
                next();
                return;
            }
            if(policy == "jwt") {
                AuthErr e = jwtAuth ? jwtAuth->authenticate(ctx) : AuthErr::BadSig;
                if(e == AuthErr::Ok && route.rule
                   && (cfg.fwdUser || route.rule->rateLimitKey == "user")) {
                    std::string sub;
                    if(!ctx.getAttr("jwt_sub", sub) || !good_user_val(sub)) {
                        e = AuthErr::BadToken;
                    }
                }
                old_auth(e);
                if(e != AuthErr::Ok) {
                    mark_auth(ctx, "jwt", 0, e, routeName);
                    reply(ctx, 401, "Unauthorized\n");
                    return;
                }
                // 认证过了再查授权
                AuthErr authz = route.rule ? authz_err(*route.rule, ctx) : AuthErr::Ok;
                if(authz != AuthErr::Ok) {
                    GatewayMetrics::instance().incr_auth_forbidden();
                    mark_auth(ctx, "jwt", 0, authz, routeName);
                    reply(ctx, 403, "Forbidden\n");
                    return;
                }
                mark_auth(ctx, "jwt", 0, AuthErr::Ok, routeName);
                if(cfg.stripBearer && ctx.request()) {
                    ctx.request()->delHeader("Authorization");
                }
                fwd_user(cfg, ctx);
                next();
                return;
            }
            if(policy == "api_key") {
                auto req = ctx.request();
                std::string key = req ? req->getHeader(cfg.apiKeyHeader) : "";
                const ApiKey* found = apikey_find(cfg.keys, key);
                if(found || apikey_ok(cfg.apiKeys, key)) {
                    if(found) {
                        ctx.setAttr("api_key_id", found->id);
                        ctx.setAttr("auth_scopes", found->scopes);
                    }
                    GatewayMetrics::instance().incr_auth_ok();
                    AuthErr authz = route.rule ? authz_err(*route.rule, ctx) : AuthErr::Ok;
                    if(authz != AuthErr::Ok) {
                        GatewayMetrics::instance().incr_auth_forbidden();
                        mark_auth(ctx, "api_key", 1, authz, routeName);
                        reply(ctx, 403, "Forbidden\n");
                        return;
                    }
                    if(cfg.stripApiKey && req) req->delHeader(cfg.apiKeyHeader);
                    mark_auth(ctx, "api_key", 1, AuthErr::Ok, routeName);
                    next();
                    return;
                }
                GatewayMetrics::instance().incr_auth_no_token();
                mark_auth(ctx, "api_key", 1,
                          key.empty() ? AuthErr::NoToken : AuthErr::BadKey, routeName);
                reply(ctx, 401, "Unauthorized\n");
                return;
            }
            mark_auth(ctx, "other", 2, AuthErr::BadPolicy, routeName);
            reply(ctx, 403, "Forbidden\n");
        }, "route_auth");
}

} // namespace gateway
} // namespace bronx
