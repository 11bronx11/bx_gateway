// 版本变更环形日志。每条规则变动 = 一个版本 = 一条记录(1:1)。网关按 ackedVersion 来补
// 增量: 要的区间还在环里就发 delta, 滚出去了就发全量。环有界, 只留最近 N 条。
#pragma once

#include "wire.h"
#include <cstdint>
#include <deque>
#include <vector>

namespace bronx {
namespace ipban {

class ChangeLog {
public:
    explicit ChangeLog(size_t cap = 4096) : m_cap(cap) {}

    struct Entry { uint64_t version; DeltaOp op; };

    // 记一条(版本已由 engine +1 算好)。超容量丢最旧。
    void append(uint64_t version, DeltaOp op) {
        m_log.push_back(Entry{version, std::move(op)});
        while(m_log.size() > m_cap) m_log.pop_front();
    }

    // 环里最旧的版本(空返回 0)。
    uint64_t oldestVersion() const { return m_log.empty() ? 0 : m_log.front().version; }

    // 能不能覆盖 (fromVer, toVer] —— 即 fromVer+1 那条还在环里。
    bool canCover(uint64_t fromVer) const {
        if(m_log.empty()) return false;
        return fromVer + 1 >= m_log.front().version;
    }

    // 收 (fromVer, toVer] 的所有 op。canCover 为真时调。
    void collect(uint64_t fromVer, uint64_t toVer, std::vector<DeltaOp>& out) const {
        for(const auto& e : m_log) {
            if(e.version > fromVer && e.version <= toVer) out.push_back(e.op);
        }
    }

    size_t size() const { return m_log.size(); }

private:
    size_t m_cap;
    std::deque<Entry> m_log;
};

} // namespace ipban
} // namespace bronx
