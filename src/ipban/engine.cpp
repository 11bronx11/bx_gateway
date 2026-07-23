#include "engine.h"
#include "judge.h"
#include "wire.h"
#include "util.h"
#include "log.h"
#include "offload.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <random>

namespace bronx {
namespace ipban {

static bronx::BxLogger::ptr g_log = BRONX_LOG_NAME("system");
static constexpr uint64_t kRiskCoolMs = 60000;
static constexpr size_t kRiskSeenMax = 10000;
static constexpr uint64_t kDbIntMax = (uint64_t)std::numeric_limits<int64_t>::max();
static constexpr uint64_t kDbUs[] = {
    1000, 5000, 10000, 25000, 50000, 100000, 250000, 500000, 1000000, UINT64_MAX
};

static size_t srcSlot(Src src) {
    size_t i = static_cast<size_t>(src);
    return i < 6 ? i : static_cast<size_t>(Src::RATE);
}

// 摇一个非零随机 epoch 当本次启动的身份
static uint64_t rollEpoch() {
    std::random_device rd;
    uint64_t e = ((uint64_t)rd() << 32) ^ rd() ^ bronx::GetCurrentMs();
    return e ? e : 1;
}

PolicyEngine::PolicyEngine() : m_epoch(rollEpoch()) {
    BRONX_LOG_INFO(g_log) << "policy engine epoch=" << m_epoch;
}

void PolicyEngine::setDb(Db::ptr db, BxCpuPool* pool, uint64_t minTtlMs) {
    std::shared_ptr<BxCpuPool> ref;
    if(pool) ref = std::shared_ptr<BxCpuPool>(pool, [](BxCpuPool*) {});
    setDbOwned(std::move(db), std::move(ref), minTtlMs);
}

void PolicyEngine::setDbOwned(Db::ptr db, std::shared_ptr<BxCpuPool> pool,
                              uint64_t minTtlMs) {
    std::lock_guard<std::mutex> lk(m_dbMtx);
    m_db = std::move(db);
    m_pool = pool;
    m_persistMinTtlMs.store(minTtlMs, std::memory_order_relaxed);
    if(!m_db) m_persistHealthy.store(true, std::memory_order_release);
}

bool PolicyEngine::dbEnabled() {
    std::lock_guard<std::mutex> lk(m_dbMtx);
    return m_db != nullptr;
}

void PolicyEngine::queueRuleLocked(const Rule& r) {
    if(!dbEnabled()) return;
    bool want = shouldPersist(r);
    bool had = m_persisted.find(r.id) != m_persisted.end();
    if(!want && !had) {
        m_dbWrites[1].fetch_add(1, std::memory_order_relaxed);
        return;
    }

    PersistOp op;
    op.id = r.id;
    op.version = r.version;
    if(want) {
        op.rule = r;
        m_persisted.insert(r.id);
    } else {
        op.del = true;
        op.op = 1;
        m_persisted.erase(r.id);
    }
    std::lock_guard<std::mutex> lk(m_pendingMtx);
    m_pending.push_back(std::move(op));
}

void PolicyEngine::queueDelLocked(const std::string& id, uint64_t ver, bool expiry) {
    if(!dbEnabled() || m_persisted.erase(id) == 0) return;
    PersistOp op;
    op.del = true;
    op.op = expiry ? 2 : 1;
    op.id = id;
    op.version = ver;
    std::lock_guard<std::mutex> lk(m_pendingMtx);
    m_pending.push_back(std::move(op));
}

size_t PolicyEngine::pendingPersistence() {
    std::lock_guard<std::mutex> lk(m_pendingMtx);
    return m_pending.size();
}

bool PolicyEngine::flushPersistence() {
    static constexpr size_t kBatch = 128;
    bool expected = false;
    if(!m_flushing.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    while(true) {
        std::vector<PersistOp> batch;
        bool empty = false;
        {
            std::lock_guard<std::mutex> lk(m_pendingMtx);
            if(m_pending.empty()) {
                empty = true;
            } else {
                size_t count = std::min(kBatch, m_pending.size());
                batch.assign(m_pending.begin(), m_pending.begin() + count);
            }
        }
        if(empty) {
            m_flushing.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> again(m_pendingMtx);
                if(m_pending.empty()) {
                    m_persistHealthy.store(true, std::memory_order_release);
                    return true;
                }
            }
            expected = false;
            if(!m_flushing.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel)) {
                return false;
            }
            continue;
        }

        size_t done = 0;
        auto began = std::chrono::steady_clock::now();
        try {
            Db::ptr db;
            std::shared_ptr<BxCpuPool> pool;
            {
                std::lock_guard<std::mutex> cfg(m_dbMtx);
                db = m_db;
                pool = m_pool;
            }
            if(!db) {
                m_persistHealthy.store(false, std::memory_order_release);
                m_flushing.store(false, std::memory_order_release);
                return false;
            }
            done = bronx::offload([db, batch]() {
                size_t count = 0;
                for(const auto& op : batch) {
                    bool ok = op.del ? db->del(op.id, op.version) : db->save(op.rule);
                    if(!ok) break;
                    ++count;
                }
                return count;
            }, pool);
        } catch(const std::exception& e) {
            BRONX_LOG_WARN(g_log) << "persist batch failed: " << e.what();
        }
        uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - began).count();
        m_dbTimeUs.fetch_add(us, std::memory_order_relaxed);
        m_dbTimeCount.fetch_add(1, std::memory_order_relaxed);
        for(size_t i = 0; i < 10; ++i) {
            if(us <= kDbUs[i]) {
                m_dbTime[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        for(size_t i = 0; i < done; ++i) {
            m_dbWrites[batch[i].op * 3].fetch_add(1, std::memory_order_relaxed);
        }
        if(done) {
            std::lock_guard<std::mutex> lk(m_pendingMtx);
            for(size_t i = 0; i < done; ++i) m_pending.pop_front();
        }
        if(done != batch.size()) {
            m_dbWrites[batch[done].op * 3 + 2].fetch_add(1, std::memory_order_relaxed);
            m_persistHealthy.store(false, std::memory_order_release);
            m_flushing.store(false, std::memory_order_release);
            const auto& failed = batch[done];
            BRONX_LOG_WARN(g_log) << "persist " << failed.id << " failed v=" << failed.version;
            return false;
        }
    }
}

// 持 m_mtx 时调: 版本+1, 记 changelog。返回新版本。
uint64_t PolicyEngine::recordLocked(DeltaOp op) {
    uint64_t v = m_ver.load(std::memory_order_relaxed) + 1;
    if(!op.del) op.rule.version = v;
    m_log.append(v, std::move(op));
    m_ver.store(v, std::memory_order_release);
    return v;
}

bool PolicyEngine::seenLocked(const Risk& r, uint64_t now) {
    if(r.id.empty()) return false;
    auto it = m_seen.find(r.id);
    if(it != m_seen.end() && now >= it->second && now - it->second < kRiskCoolMs) {
        return true;
    }
    m_seen[r.id] = now;
    m_seenOrder.emplace_back(r.id, now);
    while(!m_seenOrder.empty()) {
        const auto& old = m_seenOrder.front();
        bool expired = now >= old.second && now - old.second >= kRiskCoolMs;
        if(!expired && m_seen.size() <= kRiskSeenMax) break;
        auto found = m_seen.find(old.first);
        if(found != m_seen.end() && found->second == old.second) m_seen.erase(found);
        m_seenOrder.pop_front();
    }
    return false;
}

bool PolicyEngine::onRisk(const Risk& r) {
    uint64_t now = bronx::GetCurrentMs();
    Rule cand = ruleFromRisk(r, now);
    uint64_t newVer = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(seenLocked(r, now)) {
            m_risks[srcSlot(r.src) * 3 + 1].fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const Rule* ex = m_store.find(cand.id);
        if(ex && !shouldReplace(*ex, cand)) {
            m_risks[srcSlot(r.src) * 3 + 1].fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        DeltaOp op; op.del = false; op.ruleId = cand.id; op.rule = cand;
        newVer = recordLocked(op);
        cand.version = newVer;
        m_store.put(cand);
        m_expiry.push(cand.expireAtMs, cand.id, newVer);
        queueRuleLocked(cand);
    }
    m_risks[srcSlot(r.src) * 3].fetch_add(1, std::memory_order_relaxed);
    m_changes[srcSlot(cand.src) * 3].fetch_add(1, std::memory_order_relaxed);
    BRONX_LOG_INFO(g_log) << "rule " << cand.id << " deny src=" << srcName(cand.src)
                          << " expire=" << cand.expireAtMs << " v=" << newVer;
    flushPersistence();
    fire(newVer);
    return true;
}

EditRes PolicyEngine::setAdmin(const Ip& ip, Act action, uint64_t ttlMs,
                               const std::string& reason) {
    uint64_t now = bronx::GetCurrentMs();
    EditRes out;
    out.id = ruleIdFor(Src::ADMIN, ip);
    if(ttlMs != 0 && (now > kDbIntMax || ttlMs > kDbIntMax - now)) {
        out.version = version();
        out.error = "ttl_too_large";
        return out;
    }
    Rule rule;
    rule.id = out.id;
    rule.ip = ip;
    rule.action = action;
    rule.src = Src::ADMIN;
    rule.priority = rulePriority(rule.src, action);
    rule.createdAtMs = now;
    rule.expireAtMs = ttlMs == 0 ? 0
        : (ttlMs > std::numeric_limits<uint64_t>::max() - now
            ? std::numeric_limits<uint64_t>::max() : now + ttlMs);
    rule.reason = reason.empty() ? "admin" : reason;

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        const Rule* old = m_store.find(rule.id);
        if(old && old->action == rule.action && old->expireAtMs == rule.expireAtMs
           && old->reason == rule.reason) {
            out.version = m_ver.load(std::memory_order_relaxed);
        } else {
            DeltaOp op;
            op.ruleId = rule.id;
            op.rule = rule;
            out.version = recordLocked(op);
            rule.version = out.version;
            m_store.put(rule);
            m_expiry.push(rule.expireAtMs, rule.id, rule.version);
            queueRuleLocked(rule);
            out.changed = true;
            m_changes[srcSlot(rule.src) * 3].fetch_add(1, std::memory_order_relaxed);
        }
    }
    out.durable = flushPersistence();
    if(!out.durable) out.error = "persist_failed";
    if(!out.changed) return out;
    BRONX_LOG_INFO(g_log) << "rule " << rule.id << " " << actName(action)
                          << " expire=" << rule.expireAtMs << " v=" << out.version;
    fire(out.version);
    return out;
}

EditRes PolicyEngine::remove(const std::string& id) {
    EditRes out;
    out.id = id;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        const Rule* rule = m_store.find(id);
        if(!rule) {
            out.version = m_ver.load(std::memory_order_relaxed);
        } else {
            Src src = rule->src;
            m_store.erase(id);
            DeltaOp op;
            op.del = true;
            op.ruleId = id;
            out.version = recordLocked(op);
            queueDelLocked(id, out.version);
            out.changed = true;
            m_changes[srcSlot(src) * 3 + 1].fetch_add(1, std::memory_order_relaxed);
        }
    }
    out.durable = flushPersistence();
    if(!out.durable) out.error = "persist_failed";
    if(!out.changed) return out;
    BRONX_LOG_INFO(g_log) << "rule " << id << " removed v=" << out.version;
    fire(out.version);
    return out;
}

std::vector<Rule> PolicyEngine::rules() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_store.all();
}

void PolicyEngine::tickExpiry() {
    flushPersistence();
    uint64_t now = bronx::GetCurrentMs();
    bool changed = false;
    uint64_t lastVer = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto due = m_expiry.popDue(now);
        for(const auto& node : due) {
            const Rule* r = m_store.find(node.ruleId);
            if(!r) continue;
            if(r->version != node.version) continue;    // 被续过, 旧节点作废
            if(r->expireAtMs == 0 || now < r->expireAtMs) continue;
            Src src = r->src;
            m_store.erase(node.ruleId);
            DeltaOp op; op.del = true; op.ruleId = node.ruleId;
            lastVer = recordLocked(op);   // 逐条一个版本
            queueDelLocked(node.ruleId, lastVer, true);
            m_changes[srcSlot(src) * 3 + 2].fetch_add(1, std::memory_order_relaxed);
            changed = true;
            BRONX_LOG_INFO(g_log) << "rule " << node.ruleId << " expired v=" << lastVer;
        }
    }
    flushPersistence();
    if(changed) fire(lastVer);
}

bool PolicyEngine::nextPush(uint64_t sentVer, bool sentOnce,
                            Kind& kindOut, std::string& bodyOut, uint64_t& newVerOut) {
    std::vector<Rule> fullRules;
    std::vector<DeltaOp> ops;
    bool full = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        uint64_t cur = m_ver.load(std::memory_order_acquire);
        newVerOut = cur;
        if(sentOnce && sentVer == cur) return false;   // 没新东西
        // sentVer 超过 cur 只可能是网关报了个陈旧 epoch 的版本, 别算负区间, 直接全量
        if(!sentOnce || sentVer > cur || !m_log.canCover(sentVer)) {
            full = true;
            fullRules = m_store.all();                 // 全量: 锁内拷
        } else {
            m_log.collect(sentVer, cur, ops);          // 增量: 拷区间 op
        }
    }
    // 序列化在锁外
    if(full) {
        kindOut = Kind::SNAP;
        bodyOut = snapToJson(m_epoch, newVerOut, fullRules);
    } else {
        kindOut = Kind::DELTA;
        bodyOut = deltaToJson(m_epoch, sentVer, newVerOut, ops);
    }
    return true;
}

bool PolicyEngine::restore() {
    Db::ptr db;
    {
        std::lock_guard<std::mutex> lk(m_dbMtx);
        db = m_db;
    }
    if(!db) return true;
    uint64_t now = bronx::GetCurrentMs();
    std::vector<Rule> rules;
    uint64_t lastVer = 0;
    if(!db->load(now, rules, lastVer)) {
        m_dbRestore[2].fetch_add(1, std::memory_order_relaxed);
        m_persistHealthy.store(false, std::memory_order_release);
        BRONX_LOG_ERROR(g_log) << "policy restore: db load failed";
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    for(auto& r : rules) {
        m_store.put(r);
        m_expiry.push(r.expireAtMs, r.id, r.version);
        m_persisted.insert(r.id);
    }
    // 版本接上库里最新, 别回退。epoch 仍是新的, 网关会当重启拉全量。
    if(lastVer > m_ver.load(std::memory_order_relaxed))
        m_ver.store(lastVer, std::memory_order_release);
    m_dbRestore[0].fetch_add(1, std::memory_order_relaxed);
    m_dbRestoreRules.fetch_add(rules.size(), std::memory_order_relaxed);
    m_persistHealthy.store(true, std::memory_order_release);
    BRONX_LOG_INFO(g_log) << "policy restored rules=" << rules.size()
                          << " last_ver=" << lastVer;
    return true;
}

size_t PolicyEngine::ruleCount() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_store.size();
}
size_t PolicyEngine::expiryQueue() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_expiry.size();
}

HubStat PolicyEngine::hubStat() {
    HubStat out;
    out.version = version();
    out.rules = ruleCount();
    out.expiry = expiryQueue();
    for(size_t i = 0; i < out.risks.size(); ++i) {
        out.risks[i] = m_risks[i].load(std::memory_order_relaxed);
        out.changes[i] = m_changes[i].load(std::memory_order_relaxed);
    }
    out.db.enabled = dbEnabled();
    out.db.ok = persistenceHealthy();
    out.db.pending = pendingPersistence();
    for(size_t i = 0; i < out.db.writes.size(); ++i)
        out.db.writes[i] = m_dbWrites[i].load(std::memory_order_relaxed);
    for(size_t i = 0; i < out.db.restore.size(); ++i)
        out.db.restore[i] = m_dbRestore[i].load(std::memory_order_relaxed);
    out.db.restoreRules = m_dbRestoreRules.load(std::memory_order_relaxed);
    for(size_t i = 0; i < out.db.time.size(); ++i)
        out.db.time[i] = m_dbTime[i].load(std::memory_order_relaxed);
    out.db.timeUs = m_dbTimeUs.load(std::memory_order_relaxed);
    out.db.timeCount = m_dbTimeCount.load(std::memory_order_relaxed);
    return out;
}

} // namespace ipban
} // namespace bronx
