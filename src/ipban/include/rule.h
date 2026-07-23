#pragma once

// 两个核心数据: Risk 是"某 IP 有问题"的举报, Rule 是拍板后的封/放规则。
// 举报只是线索, 封不封多久由 daemon 的 Judge 说了算。

#include "ip.h"
#include <cstdint>
#include <string>

namespace bronx {
namespace ipban {

// 谁举报的 / 谁定的规则。Risk 只会用前几个, STATIC 和 EMERG 是规则专属。
enum class Src : uint8_t {
    WAF, RATE, BIZ, ADMIN, STATIC, EMERG
};

// 什么风险。骨干版够用即可, 不求全。
enum class RiskType : uint8_t {
    RATE_ABUSE, INJECT, SCAN, ENUM, OTHER
};

enum class Act : uint8_t { ALLOW, DENY };

// 一条举报。address+prefix 描述范围, 单 IP 就是满位。
struct Risk {
    std::string id;        // 去重键
    Ip          ip;
    Src         src = Src::RATE;
    RiskType    type = RiskType::RATE_ABUSE;
    uint32_t    severity = 1;
    uint64_t    banMs = 0;     // 建议封多久, 只是建议
    uint64_t    atMs = 0;      // 举报时刻
    std::string reason;
};

// 一条正式规则。expireAtMs=0 表示永久。priority 由 Judge 按 src+action 查表填,
// 合并时只看这个数, 数大的赢。
struct Rule {
    std::string id;
    Ip          ip;
    Act         action = Act::DENY;
    Src         src = Src::RATE;
    int32_t     priority = 0;
    uint64_t    createdAtMs = 0;
    uint64_t    expireAtMs = 0;    // 绝对过期时刻, 传绝对不传相对
    uint64_t    version = 0;       // 这条规则的版本
    std::string reason;
};

// 优先级表, 集中在这, 别散落。数大的压数小的。
inline int32_t rulePriority(Src src, Act action) {
    if(src == Src::EMERG)  return 1000;   // 人工紧急封
    if(src == Src::ADMIN)  return action == Act::DENY ? 900 : 800;
    if(src == Src::STATIC) return action == Act::ALLOW ? 750 : 300;
    if(src == Src::WAF)    return 600;
    if(src == Src::BIZ)    return 500;
    if(src == Src::RATE)   return 400;
    return 0;
}

const char* srcName(Src s);
const char* actName(Act a);
Src  srcFromName(const std::string& s);
Act  actFromName(const std::string& s);

} // namespace ipban
} // namespace bronx
