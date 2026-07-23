#pragma once

// 泛型 ARC 缓存模板，分片降锁竞争，纯 header-only，只依赖 sync.h。
// 参考论文: "ARC: A Self-Tuning, Low Overhead Replacement Cache" (Megiddo & Modha, 2003)
//
// 四个队列:
//   T1  最近访问一次（recency，LRU 行为）
//   T2  访问过多次（frequency，升级后 LRU 刷新）
//   B1  T1 驱逐的 ghost（只存 key，不存 value）
//   B2  T2 驱逐的 ghost（同上）
//
// 命中 B1 → p++（T1 目标扩大），命中 B2 → p--（T2 目标扩大）。
// 分片：N=16 shard，hash(k) & (N-1) 路由，每个 shard 独立 ARC + BxSpinLock。

#include "sync.h"
#include <list>
#include <unordered_map>
#include <atomic>
#include <cstdint>
#include <functional>

namespace bronx {

// 双链节点，挂在 list 里同时让 map 直接持迭代器，O(1) 移动/删除
template<typename K, typename V>
struct ArcNode {
    K key;
    V val;
};

// 只存 key 的 ghost 节点
template<typename K>
struct GhostNode {
    K key;
};

template<typename K, typename V, size_t N = 16>
class Arc {
public:
    static_assert(N > 0, "ARC needs at least one shard");

    explicit Arc(size_t cap) {
        if(cap == 0) return;

        m_shardCount = cap < N ? cap : N;
        size_t base = cap / m_shardCount;
        size_t extra = cap % m_shardCount;
        for(size_t i = 0; i < m_shardCount; ++i) {
            m_shards[i].cap = base + (i < extra ? 1 : 0);
        }
    }

    // 命中：更新 LRU 位置，填 out，返回 true
    bool get(const K& k, V& out) {
        if(m_shardCount == 0) {
            m_misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto& s = shard(k);
        BxSpinLock::Lock lk(s.lk);
        // T1 命中 → 升到 T2 头
        auto it1 = s.t1map.find(k);
        if(it1 != s.t1map.end()) {
            out = it1->second->val;
            auto node = *it1->second;
            s.t1.erase(it1->second);
            s.t1map.erase(it1);
            s.t2.push_front(node);
            s.t2map[k] = s.t2.begin();
            m_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // T2 命中 → 移到 T2 头
        auto it2 = s.t2map.find(k);
        if(it2 != s.t2map.end()) {
            out = it2->second->val;
            s.t2.splice(s.t2.begin(), s.t2, it2->second);
            m_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        m_misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 插入/更新，触发 replace/evict
    void put(const K& k, const V& v) {
        if(m_shardCount == 0) return;
        auto& s = shard(k);
        BxSpinLock::Lock lk(s.lk);

        size_t t1sz = s.t1.size(), t2sz = s.t2.size();
        size_t b1sz = s.b1.size(), b2sz = s.b2.size();

        // case A: key 已在 T1 或 T2
        auto it1 = s.t1map.find(k);
        if(it1 != s.t1map.end()) {
            it1->second->val = v;
            // T1 命中升到 T2 头
            auto node = *it1->second;
            s.t1.erase(it1->second);
            s.t1map.erase(it1);
            s.t2.push_front(node);
            s.t2map[k] = s.t2.begin();
            return;
        }
        auto it2 = s.t2map.find(k);
        if(it2 != s.t2map.end()) {
            it2->second->val = v;
            s.t2.splice(s.t2.begin(), s.t2, it2->second);
            return;
        }

        // case B: key 在 B1 ghost
        auto gb1 = s.b1map.find(k);
        if(gb1 != s.b1map.end()) {
            size_t delta = (b1sz >= b2sz) ? 1 : (b1sz > 0 ? b2sz / b1sz : 1);
            s.p = (s.p + delta < s.cap) ? s.p + delta : s.cap;
            do_replace(s, false);
            s.b1.erase(gb1->second);
            s.b1map.erase(gb1);
            s.t2.push_front({k, v});
            s.t2map[k] = s.t2.begin();
            return;
        }

        // case C: key 在 B2 ghost
        auto gb2 = s.b2map.find(k);
        if(gb2 != s.b2map.end()) {
            size_t delta = (b2sz >= b1sz) ? 1 : (b2sz > 0 ? b1sz / b2sz : 1);
            s.p = (s.p > delta) ? s.p - delta : 0;
            do_replace(s, true);
            s.b2.erase(gb2->second);
            s.b2map.erase(gb2);
            s.t2.push_front({k, v});
            s.t2map[k] = s.t2.begin();
            return;
        }

        // case D: 全未命中，|T1|+|B1| == cap
        if(t1sz + b1sz >= s.cap) {
            if(t1sz < s.cap) {
                // B1 驱逐最旧 ghost，再 replace
                if(!s.b1.empty()) {
                    s.b1map.erase(s.b1.back().key);
                    s.b1.pop_back();
                }
                do_replace(s, false);
            } else {
                // T1 满了，直接驱逐 T1 尾（不进 B1）
                if(!s.t1.empty()) {
                    s.t1map.erase(s.t1.back().key);
                    s.t1.pop_back();
                }
            }
            s.t1.push_front({k, v});
            s.t1map[k] = s.t1.begin();
            return;
        }

        // case E: 全未命中，T1+T2 满了
        if(t1sz + t2sz >= s.cap) {
            do_replace(s, false);
            // 防 ghost 无界膨胀: B1+B2 超 cap 时驱逐最旧 ghost
            if(s.b1.size() + s.b2.size() >= s.cap) {
                evict_ghost(s);
            }
        }
        s.t1.push_front({k, v});
        s.t1map[k] = s.t1.begin();
    }

    // 全清（reload 时调）
    void invalidate() {
        for(auto& s : m_shards) {
            BxSpinLock::Lock lk(s.lk);
            s.t1.clear(); s.t1map.clear();
            s.t2.clear(); s.t2map.clear();
            s.b1.clear(); s.b1map.clear();
            s.b2.clear(); s.b2map.clear();
            s.p = 0;
        }
    }

    void stats(uint64_t& hits, uint64_t& misses) const {
        hits   = m_hits.load(std::memory_order_relaxed);
        misses = m_misses.load(std::memory_order_relaxed);
    }

private:
    using NodeList  = std::list<ArcNode<K,V>>;
    using GhostList = std::list<GhostNode<K>>;
    using NodeIter  = typename NodeList::iterator;
    using GhostIter = typename GhostList::iterator;

    struct alignas(64) Shard {
        mutable BxSpinLock lk;
        size_t p = 0;   // T1 的目标大小（自适应参数）
        size_t cap = 0;

        NodeList  t1;
        NodeList  t2;
        GhostList b1;
        GhostList b2;

        std::unordered_map<K, NodeIter>  t1map;
        std::unordered_map<K, NodeIter>  t2map;
        std::unordered_map<K, GhostIter> b1map;
        std::unordered_map<K, GhostIter> b2map;
    };

    // replace: 按 p 决定从 T1 还是 T2 驱逐
    // from_b2=true 且 |T1|==p 时也选 T1
    void do_replace(Shard& s, bool from_b2) {
        bool take_t1 = !s.t1.empty() &&
            (s.t1.size() > s.p || (from_b2 && s.t1.size() == s.p));
        if(take_t1) {
            auto& node = s.t1.back();
            s.b1.push_front({node.key});
            s.b1map[node.key] = s.b1.begin();
            s.t1map.erase(node.key);
            s.t1.pop_back();
        } else if(!s.t2.empty()) {
            auto& node = s.t2.back();
            s.b2.push_front({node.key});
            s.b2map[node.key] = s.b2.begin();
            s.t2map.erase(node.key);
            s.t2.pop_back();
        } else if(!s.t1.empty()) {
            // T2 空时兜底
            auto& node = s.t1.back();
            s.b1.push_front({node.key});
            s.b1map[node.key] = s.b1.begin();
            s.t1map.erase(node.key);
            s.t1.pop_back();
        }
    }

    // 驱逐最旧 ghost（B1 或 B2），防幽灵列表无界增长
    void evict_ghost(Shard& s) {
        if(!s.b1.empty()) {
            s.b1map.erase(s.b1.back().key);
            s.b1.pop_back();
        } else if(!s.b2.empty()) {
            s.b2map.erase(s.b2.back().key);
            s.b2.pop_back();
        }
    }

    Shard& shard(const K& k) {
        size_t h = std::hash<K>{}(k);
        return m_shards[h % m_shardCount];
    }

    size_t  m_shardCount = 0;
    Shard   m_shards[N];
    std::atomic<uint64_t> m_hits{0};
    std::atomic<uint64_t> m_misses{0};
};

} // namespace bronx
