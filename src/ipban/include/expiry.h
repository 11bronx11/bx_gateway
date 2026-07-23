// 过期最小堆。不给每条规则起 timer, 就一个堆, 堆顶是最近要过期的。
// 规则被续期时版本会变, 旧堆节点靠版本比对作废, 不误删新规则。
#pragma once

#include <cstdint>
#include <queue>
#include <string>
#include <vector>

namespace bronx {
namespace ipban {

struct ExpiryNode {
    uint64_t    expireAtMs;
    std::string ruleId;
    uint64_t    version;   // 入堆时规则的版本, 出堆比对; 对不上说明规则已变, 作废本节点
};

class Expiry {
public:
    // 小顶堆, expireAtMs 小的在上
    void push(uint64_t expireAtMs, const std::string& ruleId, uint64_t version) {
        if(expireAtMs == 0) return;   // 永久规则不进堆
        m_heap.push(ExpiryNode{expireAtMs, ruleId, version});
    }
    bool empty() const { return m_heap.empty(); }
    // 堆顶过期时刻, 空堆返回 0
    uint64_t earliest() const { return m_heap.empty() ? 0 : m_heap.top().expireAtMs; }
    // 弹出所有 expireAtMs <= now 的节点(可能含已作废的, 调用方拿去比对版本)
    std::vector<ExpiryNode> popDue(uint64_t now) {
        std::vector<ExpiryNode> out;
        while(!m_heap.empty() && m_heap.top().expireAtMs <= now) {
            out.push_back(m_heap.top());
            m_heap.pop();
        }
        return out;
    }
    size_t size() const { return m_heap.size(); }

private:
    struct Later {
        bool operator()(const ExpiryNode& a, const ExpiryNode& b) const {
            return a.expireAtMs > b.expireAtMs;   // 小顶堆
        }
    };
    std::priority_queue<ExpiryNode, std::vector<ExpiryNode>, Later> m_heap;
};

} // namespace ipban
} // namespace bronx
