#include "judge.h"
#include <limits>

namespace bronx {
namespace ipban {

std::string ruleIdFor(Src src, const Ip& ip) {
    return std::string(srcName(src)) + ":" + ip.toString();
}

Rule ruleFromRisk(const Risk& r, uint64_t nowMs) {
    static constexpr uint64_t kMaxTime = (uint64_t)std::numeric_limits<int64_t>::max();
    Rule rule;
    rule.id          = ruleIdFor(r.src, r.ip);
    rule.ip          = r.ip;
    rule.action      = Act::DENY;               // 举报只会导致封, 放行是管理/静态的事
    rule.src         = r.src;
    rule.priority    = rulePriority(r.src, Act::DENY);
    rule.createdAtMs = nowMs;
    rule.expireAtMs  = r.banMs == 0 ? 0
        : (nowMs >= kMaxTime || r.banMs > kMaxTime - nowMs ? kMaxTime : nowMs + r.banMs);
    rule.reason      = r.reason.empty() ? std::string(srcName(r.src)) : r.reason;
    return rule;
}

bool shouldReplace(const Rule& existing, const Rule& cand) {
    // 低优先不能盖高优先
    if(cand.priority < existing.priority) return false;
    if(cand.priority > existing.priority) return true;
    // 同优先: 谁过期更晚谁赢(0=永久最大), 不让新举报缩短已有 TTL
    uint64_t ee = existing.expireAtMs == 0 ? UINT64_MAX : existing.expireAtMs;
    uint64_t ce = cand.expireAtMs == 0 ? UINT64_MAX : cand.expireAtMs;
    return ce > ee;
}

} // namespace ipban
} // namespace bronx
