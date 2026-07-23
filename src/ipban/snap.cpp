#include "snap.h"
#include <algorithm>

namespace bronx {
namespace ipban {

// a 比 b 更该赢吗: 先比优先级, 平局比谁过期更晚(0=永久最大), 再平局 DENY 压 ALLOW。
static bool beats(const Rule& a, const Rule& b) {
    if(a.priority != b.priority) return a.priority > b.priority;
    uint64_t ea = a.expireAtMs == 0 ? UINT64_MAX : a.expireAtMs;
    uint64_t eb = b.expireAtMs == 0 ? UINT64_MAX : b.expireAtMs;
    if(ea != eb) return ea > eb;
    return a.action == Act::DENY && b.action != Act::DENY;
}

static bool expired(const Rule& r, uint64_t nowMs) {
    return r.expireAtMs != 0 && nowMs != 0 && nowMs >= r.expireAtMs;
}

Snap::ptr compileSnap(uint64_t version, Act defaultAction,
                      const std::vector<Rule>& rules, uint64_t nowMs) {
    auto snap = std::make_shared<Snap>();
    snap->version = version;
    snap->defaultAction = defaultAction;

    for(const auto& r : rules) {
        if(expired(r, nowMs)) continue;   // 编译期就剔掉过期的
        bool full = (r.ip.fam == Fam::V4 && r.ip.prefix == 32)
                 || (r.ip.fam == Fam::V6 && r.ip.prefix == 128);
        if(full) {
            auto& map = r.ip.fam == Fam::V4 ? snap->v4Exact : snap->v6Exact;
            map[r.ip].push_back(r);
        } else {
            (r.ip.fam == Fam::V4 ? snap->v4Cidr : snap->v6Cidr).push_back(r);
        }
    }
    auto byPrio = [](const Rule& a, const Rule& b) { return beats(a, b); };
    auto sortExact = [&](auto& map) {
        for(auto& kv : map) {
            std::sort(kv.second.begin(), kv.second.end(), byPrio);
        }
    };
    sortExact(snap->v4Exact);
    sortExact(snap->v6Exact);
    // 网段按优先级降序, eval 扫的时候高优先在前
    std::sort(snap->v4Cidr.begin(), snap->v4Cidr.end(), byPrio);
    std::sort(snap->v6Cidr.begin(), snap->v6Cidr.end(), byPrio);
    return snap;
}

Decision Snap::eval(const Ip& ip, uint64_t nowMs) const {
    const Rule* best = nullptr;

    // 精确命中
    const auto& exact = ip.fam == Fam::V4 ? v4Exact : v6Exact;
    auto it = exact.find(ip);
    if(it != exact.end()) {
        for(const auto& r : it->second) {
            if(expired(r, nowMs)) continue;
            best = &r;
            break;
        }
    }
    // 网段命中, 已按优先级排序; 但高优先可能过期, 得继续找没过期的更优者
    const auto& cidrs = ip.fam == Fam::V4 ? v4Cidr : v6Cidr;
    for(const auto& r : cidrs) {
        if(best && r.priority < best->priority) break;   // 优先级更低的后面都不可能更优
        if(expired(r, nowMs)) continue;
        if(inNet(ip, r.ip)) {
            if(!best || beats(r, *best)) best = &r;
        }
    }

    Decision d;
    if(best) {
        d.action     = best->action;
        d.matched    = true;
        d.ruleId     = best->id;
        d.src        = best->src;
        d.expireAtMs = best->expireAtMs;
        d.reason     = best->reason;
    } else {
        d.action  = defaultAction;
        d.matched = false;
    }
    return d;
}

} // namespace ipban
} // namespace bronx
