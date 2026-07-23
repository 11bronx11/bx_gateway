#pragma once

// Risk/Rule 跨进程时的 JSON 编解码。传输格式和网关本地编译的 Snap 分开,
// 这里只管"变成一段文本"和"从文本还原", 不碰匹配加速结构。

#include "rule.h"
#include <string>
#include <vector>

namespace bronx {
namespace ipban {

// Risk <-> JSON
std::string riskToJson(const Risk& r);
bool riskFromJson(const std::string& s, Risk& out);

// Rule <-> JSON
std::string ruleToJson(const Rule& r);
bool ruleFromJson(const std::string& s, Rule& out);

// 一批 Rule + 版本 + epoch, 打成全量快照 body。
// epoch = daemon 本次启动的随机标识, 网关据此辨认对面是不是重启过(重启则版本会回退)。
std::string snapToJson(uint64_t epoch, uint64_t version, const std::vector<Rule>& rules);
bool snapFromJson(const std::string& s, uint64_t& epoch, uint64_t& version, std::vector<Rule>& out);

// gateway 打招呼: instanceId + 已应用版本 + 手里那版的 epoch(重连对账用)。
std::string helloToJson(const std::string& instanceId, uint64_t epoch, uint64_t version);
bool helloFromJson(const std::string& s, std::string& instanceId, uint64_t& epoch, uint64_t& version);

std::string readyToJson(uint64_t epoch, uint64_t version);
bool readyFromJson(const std::string& s, uint64_t& epoch, uint64_t& version);

std::string errToJson(const std::string& code);
bool errFromJson(const std::string& s, std::string& code);

// 增量: epoch + prevVer(网关须匹配的当前版本) + newVer + 一串 op。
// op 两种: upsert(带完整 rule) / del(只 ruleId)。网关 epoch 一致且 localVer==prevVer 才能应用。
struct DeltaOp {
    bool        del = false;
    std::string ruleId;
    Rule        rule;    // del 时无意义
};
std::string deltaToJson(uint64_t epoch, uint64_t prevVer, uint64_t newVer, const std::vector<DeltaOp>& ops);
bool deltaFromJson(const std::string& s, uint64_t& epoch, uint64_t& prevVer, uint64_t& newVer,
                   std::vector<DeltaOp>& ops);

struct CtlPut {
    Ip ip;
    Act action = Act::ALLOW;
    uint64_t ttlMs = 0;
    std::string reason;
};

struct CtlRes {
    bool ok = false;
    bool changed = false;
    uint64_t version = 0;
    std::string id;
    std::string error;
    std::vector<Rule> rules;
};

std::string ctlPutToJson(const CtlPut& in);
bool ctlPutFromJson(const std::string& s, CtlPut& out);
std::string ctlDelToJson(const std::string& id);
bool ctlDelFromJson(const std::string& s, std::string& id);
std::string ctlResToJson(const CtlRes& in);
bool ctlResFromJson(const std::string& s, CtlRes& out);

} // namespace ipban
} // namespace bronx
