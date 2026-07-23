#pragma once

// 规则仓库。抽象接口留着以后接 SQL, 骨干版只给内存实现。
// daemon 权威规则都存这, 按 ruleId 索引。ruleId 编码 src+ip, 同源同 IP 重复举报
// 落同一条(更新而非堆积)。

#include "rule.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bronx {
namespace ipban {

class Store {
public:
    using ptr = std::shared_ptr<Store>;
    virtual ~Store() = default;

    virtual void put(const Rule& r) = 0;
    virtual void erase(const std::string& ruleId) = 0;
    virtual const Rule* find(const std::string& ruleId) const = 0;
    virtual std::vector<Rule> all() const = 0;
    virtual size_t size() const = 0;
};

// 内存实现, 一把 map。daemon 单线程 IoManager 下够用, 多线程调用方自己护。
class MemStore : public Store {
public:
    void put(const Rule& r) override { m_map[r.id] = r; }
    void erase(const std::string& id) override { m_map.erase(id); }
    const Rule* find(const std::string& id) const override {
        auto it = m_map.find(id);
        return it == m_map.end() ? nullptr : &it->second;
    }
    std::vector<Rule> all() const override {
        std::vector<Rule> v;
        v.reserve(m_map.size());
        for(const auto& kv : m_map) v.push_back(kv.second);
        return v;
    }
    size_t size() const override { return m_map.size(); }
private:
    std::unordered_map<std::string, Rule> m_map;
};

} // namespace ipban
} // namespace bronx
