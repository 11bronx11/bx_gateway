#pragma once

// 网关本地编译好的不可变名单快照。发布前全部预编译好, 请求期只读不改。
// 单 IP(满位)进 hash 表 O(1) 查, 网段进 vector 扫。
// 同一精确 IP 可有多条规则, eval 按优先级找第一条未过期的。

#include "rule.h"
#include <memory>
#include <unordered_map>
#include <vector>

namespace bronx {
namespace ipban {

// evaluate 的结论, 够记拒绝日志用。
struct Decision {
    Act         action = Act::ALLOW;
    bool        matched = false;    // 命中了具体规则, 非兜底 default
    std::string ruleId;
    Src         src = Src::STATIC;
    uint64_t    expireAtMs = 0;
    std::string reason;
};

class Snap {
public:
    using ptr = std::shared_ptr<const Snap>;

    uint64_t version = 0;
    Act      defaultAction = Act::ALLOW;

    // 单 IP: v4=/32 v6=/128
    std::unordered_map<Ip, std::vector<Rule>, IpHash, IpEq> v4Exact;
    std::unordered_map<Ip, std::vector<Rule>, IpHash, IpEq> v6Exact;
    // 网段, 编译时已按 priority 从大到小排好
    std::vector<Rule> v4Cidr;
    std::vector<Rule> v6Cidr;

    // 查一个地址。nowMs 用来当场剔掉已过期规则(本地 TTL 兜底第二层)。
    // 命中多条取优先级最高的没过期规则, 都没命中走 defaultAction。
    Decision eval(const Ip& ip, uint64_t nowMs) const;

    size_t ruleCount() const {
        size_t n = v4Cidr.size() + v6Cidr.size();
        for(const auto& kv : v4Exact) n += kv.second.size();
        for(const auto& kv : v6Exact) n += kv.second.size();
        return n;
    }
};

// 一堆规则编译成 Snap, 同一精确 IP 的规则按优先级排好
// 已过期的规则在编译期就不收(nowMs 传 0 表示不按时间过滤, 全收)。
Snap::ptr compileSnap(uint64_t version, Act defaultAction,
                      const std::vector<Rule>& rules, uint64_t nowMs);

} // namespace ipban
} // namespace bronx
