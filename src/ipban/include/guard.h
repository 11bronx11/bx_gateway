#pragma once

// 网关侧的名单账本, 挂在 GatewayServer 上, 活得跟进程一样久, reload 不换。
// 持一份原子快照给热路径读; 静态规则(YAML)和远程规则(daemon)分开存, 各自更新后
// 合并重编快照原子换上。daemon 断了也接着用最后一版, 本地按 expireAt 自己兜底过期。

#include "snap.h"
#include "wire.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bronx {
namespace ipban {

struct StaticPolicy {
    bool enabled = false;
    Act def = Act::ALLOW;
    std::vector<Rule> rules;
};

class Guard {
public:
    using ptr = std::shared_ptr<Guard>;

    Guard();

    // 热路径唯一入口: 原子拿快照, 查一次。ip 是规范化后的客户端地址。
    Decision eval(const Ip& ip) const;

    // 静态规则开关, 默认关。
    void setEnabled(bool v) { m_staticEnabled.store(v, std::memory_order_relaxed); }
    // 远端同步开关, 配置重载不能把它关掉。
    void setRemoteEnabled(bool v) { m_remoteEnabled.store(v, std::memory_order_relaxed); }
    bool enabled() const {
        return m_staticEnabled.load(std::memory_order_relaxed)
            || m_remoteEnabled.load(std::memory_order_relaxed);
    }

    // 兜底动作: 没命中任何规则时的默认(denylist=ALLOW, allowlist=DENY)。
    void setDefaultAction(Act a);

    // 静态规则整批替换(启动 bootstrap / reload 调)。不动远程规则, 重编快照。
    void setStatic(std::vector<Rule> rules);
    // 默认动作+静态规则一把换(reload 用)。一次锁一次重编, 没有"新默认配旧规则"的瞬态。
    void applyStatic(Act def, std::vector<Rule> rules);
    void applyStatic(StaticPolicy policy);
    // 远程规则整批替换(收到全量 SNAP 调)。epoch 变了(daemon 重启)无条件重置; epoch 同则
    // 只认更新的版本, 陈旧/乱序的忽略。不动静态规则。
    bool applyRemote(uint64_t epoch, uint64_t version, std::vector<Rule> rules);
    // 增量应用(收到 DELTA 调)。epoch 一致且本地版本==prevVer 才应用, 否则返回 false 让调用方拉全量。
    bool applyDelta(uint64_t epoch, uint64_t prevVer, uint64_t newVer, const std::vector<DeltaOp>& ops);

    // 本地过期兜底: 有规则到期就重编快照(定时器周期调)。返回是否重编了。
    bool tickExpiry();

    // 当前快照(调试/metrics)。热路径别频繁调, 用 eval。
    Snap::ptr snapshot() const {
        return m_snap.load(std::memory_order_acquire);
    }
    uint64_t remoteVersion() const { return m_remoteVer.load(std::memory_order_relaxed); }
    // 手里这版规则来自哪个 daemon 实例, 重连 HELLO 带上给对面对账。
    uint64_t remoteEpoch() const { return m_remoteEpoch.load(std::memory_order_relaxed); }
    size_t staticCount() const;
    size_t remoteCount() const;

private:
    // 合并静态+远程重编快照原子换。持 m_mtx 调。
    void rebuildLocked();
    // 下一个最近过期时刻(0=无), 决定 tickExpiry 有没有活干。持 m_mtx 调。
    uint64_t nextExpiryLocked() const;

private:
    std::atomic<std::shared_ptr<const Snap>> m_snap;   // 热路径读
    std::atomic<bool> m_staticEnabled{false};
    std::atomic<bool> m_remoteEnabled{false};

    mutable std::mutex m_mtx;        // 护 static/remote/default, 只在更新时抢, 不进热路径
    Act m_default = Act::ALLOW;
    std::vector<Rule> m_static;
    std::unordered_map<std::string, Rule> m_remote;   // 按 ruleId, 支撑 delta 增删改
    std::atomic<uint64_t> m_remoteVer{0};
    std::atomic<uint64_t> m_remoteEpoch{0};   // 手里这版规则来自哪个 daemon 实例
    uint64_t m_snapVer = 0;          // 递增给 Snap 打标, 纯本地编号
    uint64_t m_nextExpiry = 0;       // 最近一条规则的过期时刻, 重编时算好, tick 直接读省全表扫
};

// 把旧的静态 ip_filter 配置(mode + cidrs)转成规则。denylist: 命中 DENY 默认放行;
// allowlist: 命中 ALLOW 默认拒。返回规则 + 该用的兜底动作。
struct StaticFilterCfg {
    bool enabled = false;
    std::string mode = "denylist";        // denylist / allowlist
    std::vector<std::string> cidrs;
};
// skipped 非空时带出被跳过的非法 cidr 条数, 调用方好告警。
std::vector<Rule> staticRulesFromCfg(const StaticFilterCfg& cfg, Act& defaultOut,
                                     size_t* skipped = nullptr);

} // namespace ipban
} // namespace bronx
