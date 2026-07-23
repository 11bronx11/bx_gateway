// 策略引擎: 收举报 -> Judge 仲裁 -> Store 存 -> 版本+1 记 changelog, 过期扫堆也走这。
// 规则状态的唯一权威。每条规则变动 = 一个版本 = 一条 changelog(1:1), 支撑增量补发。
// 改了就 fire onChange 让 Broadcaster 去推。锁只护 store/expiry/changelog, 序列化挪锁外。
#pragma once

#include "store.h"
#include "expiry.h"
#include "changelog.h"
#include "proto.h"
#include "db.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bronx {
class BxCpuPool;
namespace ipban {

struct EditRes {
    bool changed = false;
    bool durable = true;
    uint64_t version = 0;
    std::string id;
    std::string error;
};

struct DbStat {
    bool enabled = false;
    bool ok = true;
    uint64_t pending = 0;
    std::array<uint64_t, 9> writes{};
    std::array<uint64_t, 3> restore{};
    uint64_t restoreRules = 0;
    std::array<uint64_t, 10> time{};
    uint64_t timeUs = 0;
    uint64_t timeCount = 0;
};

struct HubStat {
    bool up = false;
    uint64_t version = 0;
    uint64_t rules = 0;
    uint64_t expiry = 0;
    uint64_t sessions = 0;
    uint64_t sessionsPeak = 0;
    std::array<uint64_t, 3> links{};
    std::array<uint64_t, 5> linkErr{};
    std::array<uint64_t, 18> risks{};
    std::array<uint64_t, 18> changes{};
    DbStat db;
};

class PolicyEngine {
public:
    PolicyEngine();

    using OnChange = std::function<void(uint64_t version)>;
    void setOnChange(OnChange cb) { m_onChange = std::move(cb); }

    // 挂持久化(可空=不落盘, 行为同以前)。pool 给 offload 用, 把 db 写挪出 reactor。
    // minTtlMs: 只落"值得重启后恢复"的封禁 —— 永久 or 原始时长>=这个阈值。
    // 比它短的临时封(限流那种)不落盘: daemon 重启窗口内它多半已过期, 落了也白落。
    void setDb(Db::ptr db, BxCpuPool* pool, uint64_t minTtlMs = 300000);
    void setDbOwned(Db::ptr db, std::shared_ptr<BxCpuPool> pool,
                    uint64_t minTtlMs = 300000);
    // 启动从 db 灌回规则, 版本接上不回退。
    bool restore();
    bool flushPersistence();
    bool persistenceHealthy() const { return m_persistHealthy.load(std::memory_order_acquire); }
    size_t pendingPersistence();

    bool onRisk(const Risk& r);   // 仲裁进 store, 变了版本+1 记 changelog, 落盘, fire
    EditRes setAdmin(const Ip& ip, Act action, uint64_t ttlMs,
                     const std::string& reason);
    EditRes remove(const std::string& id);
    std::vector<Rule> rules();
    void tickExpiry();            // 到期的逐条删, 每条 +1 版本记 changelog, 落盘, fire 一次

    uint64_t version() const { return m_ver.load(std::memory_order_acquire); }
    // 本次启动的随机标识, 进程活多久不变。网关靠它辨认 daemon 重启过没(重启后版本会回退)。
    uint64_t epoch() const { return m_epoch; }

    // 给一个网关算下一步发什么。sentVer=它已应用到的版本, sentOnce=是否发过全量。
    // 返回 false=没新东西。否则填 kind(SNAP/DELTA) + body + newVer。
    // sentVer 落在 changelog 窗口内发 DELTA, 滚出去了或没发过就发全量 SNAP。
    bool nextPush(uint64_t sentVer, bool sentOnce,
                  Kind& kindOut, std::string& bodyOut, uint64_t& newVerOut);

    size_t ruleCount();
    size_t expiryQueue();
    HubStat hubStat();

private:
    // 锁内: 记一条变更(版本+1 + changelog)。返回新版本。
    uint64_t recordLocked(DeltaOp op);
    bool seenLocked(const Risk& r, uint64_t now);
    void fire(uint64_t v) { if(m_onChange) m_onChange(v); }
    struct PersistOp {
        bool del = false;
        uint8_t op = 0;
        Rule rule;
        std::string id;
        uint64_t version = 0;
    };
    void queueRuleLocked(const Rule& r);
    void queueDelLocked(const std::string& id, uint64_t ver, bool expiry = false);
    bool dbEnabled();
    // 这条封禁值不值得落盘: 永久 or 原始时长>=阈值。onRisk/tickExpiry 两端同一判据,
    // 保证短封两端都跳过(没写过也别去删)。
    bool shouldPersist(const Rule& r) const {
        if(r.src == Src::ADMIN || r.src == Src::EMERG) return true;
        if(r.expireAtMs == 0) return true;                 // 永久封, 必落
        if(r.expireAtMs <= r.createdAtMs) return false;    // 兜底: 时长非正
        return (r.expireAtMs - r.createdAtMs)
            >= m_persistMinTtlMs.load(std::memory_order_relaxed);
    }

    std::mutex            m_mtx;
    MemStore              m_store;
    Expiry                m_expiry;
    ChangeLog             m_log;
    std::unordered_map<std::string, uint64_t> m_seen;
    std::deque<std::pair<std::string, uint64_t>> m_seenOrder;
    std::unordered_set<std::string> m_persisted;
    std::atomic<uint64_t> m_ver{0};
    const uint64_t        m_epoch;   // 构造时摇一次, 之后只读
    OnChange              m_onChange;
    std::mutex            m_dbMtx;
    Db::ptr               m_db;              // 可空, 不挂就不落盘
    std::shared_ptr<BxCpuPool> m_pool;
    std::atomic<uint64_t> m_persistMinTtlMs{300000};  // 短于此的临时封不落盘
    std::mutex            m_pendingMtx;
    std::deque<PersistOp> m_pending;
    std::atomic<bool>     m_flushing{false};
    std::atomic<bool>     m_persistHealthy{true};
    std::array<std::atomic<uint64_t>, 18> m_risks{};
    std::array<std::atomic<uint64_t>, 18> m_changes{};
    std::array<std::atomic<uint64_t>, 9> m_dbWrites{};
    std::array<std::atomic<uint64_t>, 3> m_dbRestore{};
    std::atomic<uint64_t> m_dbRestoreRules{0};
    std::array<std::atomic<uint64_t>, 10> m_dbTime{};
    std::atomic<uint64_t> m_dbTimeUs{0};
    std::atomic<uint64_t> m_dbTimeCount{0};
};

} // namespace ipban
} // namespace bronx
