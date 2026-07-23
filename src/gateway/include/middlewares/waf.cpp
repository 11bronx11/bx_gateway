#include "waf.h"
#include "report.h"
#include "ctx.h"
#include "metrics.h"
#include "util.h"
#include "log.h"
#include <cstring>
#include <regex>
#include <strings.h>
#include <utility>
#include <vector>

namespace bronx {
namespace ipban {

using namespace bronx::gateway;

static bronx::BxLogger::ptr g_log = BRONX_LOG_NAME("system");

// 一条规则 名字 + 编译好的正则。名字进日志和 risk.reason, 好回溯是哪条命中。
struct Rule2 {
    const char* name;
    std::regex  re;
};

// 内置规则集, 起手几类高价值的, 后面要扩就往这加。大小写不敏感。
// 只找特征串 不求语义完整, 轻量优先。
static const std::vector<Rule2>& rules() {
    static const std::vector<Rule2> r = [] {
        auto ic = std::regex::icase | std::regex::optimize;
        std::vector<Rule2> v;
        v.push_back({"sqli",   std::regex(R"((union\s+select|or\s+1\s*=\s*1|';|--\s|/\*|sleep\s*\(|benchmark\s*\())", ic)});
        v.push_back({"xss",    std::regex(R"((<script|javascript:|onerror\s*=|onload\s*=|<img[^>]+src))", ic)});
        // 路径穿越: 直接匹配常见模式
        v.push_back({"travers",std::regex(R"((\.\./|\.\.\\|%2e%2e/|/etc/passwd|c:\\windows))", ic)});
        // 扫描器: 简单字符串匹配
        v.push_back({"scanner",std::regex(R"((sqlmap|nikto|nmap|masscan|acunetix|nessus|zgrab))", ic)});
        return v;
    }();
    return r;
}

// 扫一段文本 命中就返回规则名, 没中返回 nullptr。
static const char* scan(const std::string& s) {
    if(s.empty()) return nullptr;
    for(const auto& r : rules()) {
        if(std::regex_search(s, r.re)) return r.name;
    }
    return nullptr;
}

static int hex(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string unescape(const std::string& s, bool plusSpace) {
    std::string out;
    out.reserve(s.size());
    for(size_t i = 0; i < s.size(); ++i) {
        if(s[i] == '%' && i + 2 < s.size()) {
            int hi = hex(s[i + 1]);
            int lo = hex(s[i + 2]);
            if(hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(plusSpace && s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

static const char* scanUrl(const std::string& s, bool plusSpace) {
    std::string cur = s;
    // 最多解两层, 常见套娃能挡住。
    for(int i = 0; i <= 2; ++i) {
        if(auto hit = scan(cur)) return hit;
        if(i == 2) break;
        std::string next = unescape(cur, plusSpace);
        if(next == cur) break;
        cur = std::move(next);
    }
    return nullptr;
}

static bool watchedHeader(const std::string& key) {
    return strcasecmp(key.c_str(), "User-Agent") == 0
        || strcasecmp(key.c_str(), "Referer") == 0
        || strcasecmp(key.c_str(), "Cookie") == 0;
}

// 扫这个请求的 url + 几个常被塞攻击串的 header。中了返回规则名。
static const char* inspect(ReqCtx& ctx) {
    auto req = ctx.request();
    if(!req) return nullptr;
    if(auto hit = scanUrl(req->getPath(), false)) return hit;
    if(auto hit = scanUrl(req->getQuery(), true)) return hit;
    for(const auto& h : req->getHeaderList()) {
        if(watchedHeader(h.first)) {
            if(auto hit = scanUrl(h.second, false)) return hit;
        }
    }
    return nullptr;
}

static size_t ruleSlot(const char* hit) {
    if(strcmp(hit, "sqli") == 0) return 0;
    if(strcmp(hit, "xss") == 0) return 1;
    if(strcmp(hit, "travers") == 0) return 2;
    return 3;
}

bronx::gateway::Middleware::ptr MakeWaf(WafCfg cfg,
                                        std::shared_ptr<Reporter> reporter,
                                        std::vector<Ip> trustedProxies) {
    return std::make_shared<FuncMiddleware>(
        [cfg, reporter, trusted = std::move(trustedProxies)]
        (ReqCtx& ctx, const NextFn& next) {
            if(!cfg.on) { next(); return; }
            const char* hit = inspect(ctx);
            if(!hit) { next(); return; }

            GatewayMetrics::instance().incr_waf_denied();
            bool haveIp = loadClientAddr(ctx, trusted);
            Ip ip;
            if(haveIp) ip = ctx.clientAddr().client;
            BRONX_LOG_WARN(g_log) << "waf deny rule=" << hit
                << " ip=" << (haveIp ? ip.toString() : "unknown")
                << " path=" << (ctx.request() ? ctx.request()->getPath() : "");

            // 报一条坏 IP 给 daemon, 攻击往往接着来, 让它进封禁池。
            bool reported = false;
            if(reporter && haveIp) {
                Risk r;
                r.id = ip.toString() + "@waf";   // 同 IP 的 waf 命中去重, 别刷屏
                r.ip = ip;
                r.src = Src::WAF;
                r.type = RiskType::INJECT;
                r.banMs = cfg.banMs;
                r.atMs = bronx::GetCurrentMs();
                r.reason = std::string("waf ") + hit;
                reported = reporter->tryReport(r);
            }
            GatewayMetrics::instance().note_waf(ruleSlot(hit), reported);
            reply(ctx, 403, "Forbidden\n", {}, true);
        }, "waf");
}

} // namespace ipban
} // namespace bronx
