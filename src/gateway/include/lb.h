#pragma once

// 从一组 endpoint 里挑一个，select 返回选中的下标，endpoint 由 UpstreamGroup 管。
// 内置轮询，加权轮询，最少连接三种，一致性哈希先占个位没实现。

#include <vector>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace bronx {
namespace gateway {

constexpr uint32_t kMaxLoadBalancerWeight = 1000000;

// 供 LB 读的 endpoint 信息快照（避免 LB 持有完整 Endpoint 指针）
struct LbCand {
    size_t   index;        // 在 UpstreamGroup::m_endpoints 中的位置
    uint32_t weight;
    int32_t  activeConns;  // 当前活跃连接数（least-conn 用）
};

class LoadBalancer {
public:
    using ptr = std::shared_ptr<LoadBalancer>;
    virtual ~LoadBalancer() = default;
    // 从 candidates 中选一个，返回其 index；candidates 为空时返回 SIZE_MAX。
    virtual size_t select(const std::vector<LbCand>& candidates) = 0;
    virtual const char* name() const = 0;
};

// Round Robin
class RrLb : public LoadBalancer {
public:
    const char* name() const override { return "round_robin"; }
    size_t select(const std::vector<LbCand>& c) override {
        if(c.empty()) return SIZE_MAX;
        return c[m_counter.fetch_add(1, std::memory_order_relaxed) % c.size()].index;
    }
private:
    std::atomic<uint64_t> m_counter{0};
};

// Weighted Round Robin（平滑版）
// 每次选当前权重最高的 candidate，选后减 total_weight，其它均加 weight。
class WeightedRr : public LoadBalancer {
public:
    const char* name() const override { return "weighted"; }
    size_t select(const std::vector<LbCand>& c) override;
private:
    std::mutex          m_mutex;
    std::vector<int64_t> m_cur; // 当前权重（与 candidates 对位，按 candidates.size() 动态调）
    std::vector<size_t>  m_indexes;
    std::vector<uint32_t> m_weights;
    uint64_t             m_zero_wt_cnt = 0;
};

// Least Connections
class LeastConn : public LoadBalancer {
public:
    const char* name() const override { return "least_conn"; }
    size_t select(const std::vector<LbCand>& c) override {
        if(c.empty()) return SIZE_MAX;
        int32_t minConn = c[0].activeConns;
        for(size_t i = 1; i < c.size(); ++i) {
            if(c[i].activeConns < minConn) minConn = c[i].activeConns;
        }
        // activeConns 相同的 endpoint 不能永远选第一个，否则低并发/冷启动
        // 场景会形成热点；用原子轮转做稳定 tie-break。
        size_t same = 0;
        for(const auto& x : c) {
            if(x.activeConns == minConn) ++same;
        }
        size_t pick = m_counter.fetch_add(1, std::memory_order_relaxed) % same;
        for(const auto& x : c) {
            if(x.activeConns == minConn && pick-- == 0) return x.index;
        }
        return c[0].index;
    }
private:
    std::atomic<uint64_t> m_counter{0};
};

class WeightLeast : public LoadBalancer {
public:
    const char* name() const override { return "weighted_least_conn"; }
    size_t select(const std::vector<LbCand>& c) override {
        if(c.empty()) return SIZE_MAX;
        size_t best = 0;
        for(size_t i = 1; i < c.size(); ++i) {
            uint64_t a = (uint64_t)(c[i].activeConns < 0 ? 0 : c[i].activeConns)
                       * (uint64_t)(c[best].weight ? c[best].weight : 1);
            uint64_t b = (uint64_t)(c[best].activeConns < 0 ? 0 : c[best].activeConns)
                       * (uint64_t)(c[i].weight ? c[i].weight : 1);
            if(a < b) best = i;
        }
        size_t same = 0;
        uint64_t bestConn = (uint64_t)(c[best].activeConns < 0 ? 0 : c[best].activeConns);
        uint64_t bestWeight = c[best].weight ? c[best].weight : 1;
        for(const auto& x : c) {
            uint64_t conn = (uint64_t)(x.activeConns < 0 ? 0 : x.activeConns);
            uint64_t weight = x.weight ? x.weight : 1;
            if(conn * bestWeight == bestConn * weight) ++same;
        }
        size_t pick = m_counter.fetch_add(1, std::memory_order_relaxed) % same;
        for(const auto& x : c) {
            uint64_t conn = (uint64_t)(x.activeConns < 0 ? 0 : x.activeConns);
            uint64_t weight = x.weight ? x.weight : 1;
            if(conn * bestWeight == bestConn * weight && pick-- == 0) return x.index;
        }
        return c[best].index;
    }
private:
    std::atomic<uint64_t> m_counter{0};
};

// ConsistentHash 接口占位(impl N+1)
class ChashLb : public LoadBalancer {
public:
    const char* name() const override { return "consistent_hash"; }
    size_t select(const std::vector<LbCand>& c) override {
        // placeholder：退化为 round-robin
        if(c.empty()) return SIZE_MAX;
        return c[m_counter.fetch_add(1, std::memory_order_relaxed) % c.size()].index;
    }
private:
    std::atomic<uint64_t> m_counter{0};
};

bool IsSupportedLoadBalancer(const std::string& type);
LoadBalancer::ptr MakeLoadBalancer(const std::string& type);

} // namespace gateway
} // namespace bronx
