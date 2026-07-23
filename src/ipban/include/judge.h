// 仲裁: 举报变规则 + 同一条规则的更新取舍。集中在这, 别散到网络/仓库代码里。
// 跨来源(不同 src 打同一 IP)的最终裁决在网关 Snap.eval 按优先级做, 这里只管
// 单条规则的生成和"新举报要不要盖掉旧的同源规则"。
#pragma once

#include "rule.h"
#include <string>

namespace bronx {
namespace ipban {

// 同源同 IP 落同一 ruleId, 重复举报更新而非堆积。
std::string ruleIdFor(Src src, const Ip& ip);

// 一条举报变一条候选规则(DENY, 优先级查表, 过期=now+建议时长, 0 表示没给就默认永久?
// 骨干版: banMs=0 当永久封, 有值就 now+banMs)。
Rule ruleFromRisk(const Risk& r, uint64_t nowMs);

// 已有同 key 规则时, 候选该不该盖掉它。规则: 低优先不能盖高优先, 也不能缩短其 TTL;
// 同优先取更晚过期。返回 true = 用候选替换。
bool shouldReplace(const Rule& existing, const Rule& cand);

} // namespace ipban
} // namespace bronx
