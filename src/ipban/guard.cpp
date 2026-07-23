#include "guard.h"
#include "metrics.h"
#include "util.h"
#include <algorithm>

namespace bronx {
namespace ipban {

Guard::Guard() {
    // 起手空快照, 免得热路径拿到 null
    m_snap.store(compileSnap(0, Act::ALLOW, {}, 0), std::memory_order_release);
}

Decision Guard::eval(const Ip& ip) const {
    if(!enabled()) {
        Decision d; d.action = Act::ALLOW; return d;
    }
    auto snap = m_snap.load(std::memory_order_acquire);
    return snap->eval(ip, bronx::GetCurrentMs());
}

void Guard::setDefaultAction(Act a) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_default = a;
    rebuildLocked();
}

void Guard::setStatic(std::vector<Rule> rules) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_static = std::move(rules);
    rebuildLocked();
}

void Guard::applyStatic(Act def, std::vector<Rule> rules) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_default = def;
    m_static = std::move(rules);
    rebuildLocked();
}

void Guard::applyStatic(StaticPolicy policy) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_default = policy.def;
    m_static = std::move(policy.rules);
    rebuildLocked();
    m_staticEnabled.store(policy.enabled, std::memory_order_relaxed);
}

bool Guard::applyRemote(uint64_t epoch, uint64_t version, std::vector<Rule> rules) {
    std::lock_guard<std::mutex> lk(m_mtx);
    uint64_t myEpoch = m_remoteEpoch.load(std::memory_order_relaxed);
    bool sameEpoch = (myEpoch == epoch);
    // 同一个 daemon 实例, 只认更新的版本。
    // epoch 变了说明 daemon 重启, 版本会回退, 此时无条件收下这版全量并重置对账基线。
    if(sameEpoch && version <= m_remoteVer.load(std::memory_order_relaxed)) return false;
    m_remote.clear();
    for(auto& r : rules) {
        if(!r.id.empty()) m_remote[r.id] = std::move(r);
    }
    m_remoteVer.store(version, std::memory_order_relaxed);
    m_remoteEpoch.store(epoch, std::memory_order_relaxed);
    rebuildLocked();
    return true;
}

bool Guard::applyDelta(uint64_t epoch, uint64_t prevVer, uint64_t newVer, const std::vector<DeltaOp>& ops) {
    std::lock_guard<std::mutex> lk(m_mtx);
    // epoch 或版本对不上就别打, 让调用方拉全量兜底(防跨实例/漏中间增量导致规则集撕裂)
    if(m_remoteEpoch.load(std::memory_order_relaxed) != epoch) return false;
    if(m_remoteVer.load(std::memory_order_relaxed) != prevVer) return false;
    if(newVer <= prevVer) return false;
    for(const auto& op : ops) {
        if(op.ruleId.empty()) return false;
        if(!op.del && op.rule.id != op.ruleId) return false;
    }
    for(const auto& op : ops) {
        if(op.del) m_remote.erase(op.ruleId);
        else       m_remote[op.ruleId] = op.rule;
    }
    m_remoteVer.store(newVer, std::memory_order_relaxed);
    rebuildLocked();
    return true;
}

bool Guard::tickExpiry() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if(m_nextExpiry == 0) return false;             // 没有带过期的规则
    uint64_t now = bronx::GetCurrentMs();
    if(now < m_nextExpiry) return false;            // 还没到最近一条
    auto gone = [now](const Rule& r) {
        return r.expireAtMs != 0 && r.expireAtMs <= now;
    };
    m_static.erase(std::remove_if(m_static.begin(), m_static.end(), gone), m_static.end());
    for(auto it = m_remote.begin(); it != m_remote.end();) {
        if(gone(it->second)) it = m_remote.erase(it);
        else ++it;
    }
    rebuildLocked();
    return true;
}

void Guard::rebuildLocked() {
    std::vector<Rule> all;
    all.reserve(m_static.size() + m_remote.size());
    all.insert(all.end(), m_static.begin(), m_static.end());
    for(const auto& kv : m_remote) all.push_back(kv.second);
    uint64_t now = bronx::GetCurrentMs();
    auto snap = compileSnap(++m_snapVer, m_default, all, now);
    m_snap.store(snap, std::memory_order_release);
    m_nextExpiry = nextExpiryLocked();   // 顺手算好下次过期时刻, tick 白拿
    gateway::GatewayMetrics::instance().set_ipban_rules(m_static.size(), m_remote.size());
}

uint64_t Guard::nextExpiryLocked() const {
    uint64_t best = 0;
    auto consider = [&](uint64_t exp) {
        if(exp == 0) return;
        if(best == 0 || exp < best) best = exp;
    };
    for(const auto& r : m_static)  consider(r.expireAtMs);
    for(const auto& kv : m_remote) consider(kv.second.expireAtMs);
    return best;
}

size_t Guard::staticCount() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_static.size();
}
size_t Guard::remoteCount() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_remote.size();
}

std::vector<Rule> staticRulesFromCfg(const StaticFilterCfg& cfg, Act& defaultOut,
                                     size_t* skipped) {
    if(skipped) *skipped = 0;
    if(!cfg.enabled) {
        defaultOut = Act::ALLOW;
        return {};
    }
    bool allow = (cfg.mode == "allowlist");
    defaultOut = allow ? Act::DENY : Act::ALLOW;
    std::vector<Rule> out;
    int n = 0;
    for(const auto& c : cfg.cidrs) {
        Ip ip;
        if(!parseCidr(c, ip)) {   // 非法 cidr 跳过并计数, 调用方告警
            if(skipped) ++*skipped;
            continue;
        }
        Rule r;
        r.id = "static-" + std::to_string(n++);
        r.ip = ip;
        r.action = allow ? Act::ALLOW : Act::DENY;
        r.src = Src::STATIC;
        r.priority = rulePriority(Src::STATIC, r.action);
        r.reason = "static ip_filter";
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace ipban
} // namespace bronx
