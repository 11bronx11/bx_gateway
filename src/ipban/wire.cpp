#include "wire.h"
#include <json/json.h>
#include <sstream>
#include <unordered_set>

namespace bronx {
namespace ipban {

static std::string dump(const Json::Value& v) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";   // 紧凑一行
    return Json::writeString(b, v);
}

static bool load(const std::string& s, Json::Value& out) {
    Json::CharReaderBuilder b;
    std::string err;
    std::istringstream is(s);
    return Json::parseFromStream(b, is, &out, &err);
}

// Ip 在 wire 上就是 CIDR 字符串, 收端用 parseCidr 还原(带规范化)
static std::string ipStr(const Ip& ip) { return ip.toString(); }

// 举报来源只能是实际的举报方，规则专属来源不能由 submit 协议伪造。
static bool riskSrcFromNode(const Json::Value& j, Src& out) {
    if(!j.isString()) return false;
    const std::string& s = j.asString();
    if(s == "waf")  { out = Src::WAF; return true; }
    if(s == "rate") { out = Src::RATE; return true; }
    if(s == "biz")  { out = Src::BIZ; return true; }
    return false;
}

std::string riskToJson(const Risk& r) {
    Json::Value j;
    j["id"]       = r.id;
    j["ip"]       = ipStr(r.ip);
    j["src"]      = srcName(r.src);
    j["severity"] = (Json::UInt)r.severity;
    j["ban_ms"]   = (Json::UInt64)r.banMs;
    j["at_ms"]    = (Json::UInt64)r.atMs;
    j["type"]     = (int)r.type;
    j["reason"]   = r.reason;
    return dump(j);
}

bool riskFromJson(const std::string& s, Risk& out) {
    Json::Value j;
    if(!load(s, j) || !j.isObject()) return false;
    out.id = j["id"].asString();
    if(!parseCidr(j["ip"].asString(), out.ip)) return false;
    if(!riskSrcFromNode(j["src"], out.src)) return false;
    out.severity = j["severity"].asUInt();
    out.banMs    = j["ban_ms"].asUInt64();
    out.atMs     = j["at_ms"].asUInt64();
    out.type     = (RiskType)j["type"].asInt();
    out.reason   = j["reason"].asString();
    return true;
}

static Json::Value ruleToNode(const Rule& r) {
    Json::Value j;
    j["id"]         = r.id;
    j["ip"]         = ipStr(r.ip);
    j["action"]     = actName(r.action);
    j["src"]        = srcName(r.src);
    j["priority"]   = r.priority;
    j["created_ms"] = (Json::UInt64)r.createdAtMs;
    j["expire_ms"]  = (Json::UInt64)r.expireAtMs;
    j["version"]    = (Json::UInt64)r.version;
    j["reason"]     = r.reason;
    return j;
}

static bool ruleFromNode(const Json::Value& j, Rule& out) {
    if(!j.isObject()) return false;
    if(!j["id"].isString() || j["id"].asString().empty()) return false;
    out.id = j["id"].asString();
    if(!j["ip"].isString() || !parseCidr(j["ip"].asString(), out.ip)) return false;
    if(!j["action"].isString() || !j["src"].isString()) return false;
    const std::string& action = j["action"].asString();
    const std::string& src = j["src"].asString();
    if(action == "allow") out.action = Act::ALLOW;
    else if(action == "deny") out.action = Act::DENY;
    else return false;
    if(src == "waf") out.src = Src::WAF;
    else if(src == "rate") out.src = Src::RATE;
    else if(src == "biz") out.src = Src::BIZ;
    else if(src == "admin") out.src = Src::ADMIN;
    else if(src == "static") out.src = Src::STATIC;
    else if(src == "emerg") out.src = Src::EMERG;
    else return false;
    if((out.src == Src::WAF || out.src == Src::RATE || out.src == Src::BIZ
        || out.src == Src::EMERG) && out.action != Act::DENY) return false;
    if(!j["priority"].isInt() || !j["created_ms"].isUInt64()
       || !j["expire_ms"].isUInt64() || !j["version"].isUInt64()
       || !j["reason"].isString()) return false;
    out.priority    = rulePriority(out.src, out.action);
    out.createdAtMs = j["created_ms"].asUInt64();
    out.expireAtMs  = j["expire_ms"].asUInt64();
    out.version     = j["version"].asUInt64();
    out.reason      = j["reason"].asString();
    return true;
}

std::string ruleToJson(const Rule& r) { return dump(ruleToNode(r)); }

bool ruleFromJson(const std::string& s, Rule& out) {
    Json::Value j;
    if(!load(s, j)) return false;
    return ruleFromNode(j, out);
}

std::string snapToJson(uint64_t epoch, uint64_t version, const std::vector<Rule>& rules) {
    Json::Value j;
    j["epoch"]   = (Json::UInt64)epoch;
    j["version"] = (Json::UInt64)version;
    Json::Value arr(Json::arrayValue);
    for(const auto& r : rules) arr.append(ruleToNode(r));
    j["rules"] = arr;
    return dump(j);
}

bool snapFromJson(const std::string& s, uint64_t& epoch, uint64_t& version, std::vector<Rule>& out) {
    Json::Value j;
    if(!load(s, j) || !j.isObject()) return false;
    epoch   = j["epoch"].asUInt64();
    version = j["version"].asUInt64();
    out.clear();
    const Json::Value& arr = j["rules"];
    if(!arr.isArray()) return false;
    std::unordered_set<std::string> ids;
    for(const auto& node : arr) {
        Rule r;
        if(!ruleFromNode(node, r)) return false;
        if(!ids.insert(r.id).second) return false;
        out.push_back(std::move(r));
    }
    return true;
}

std::string helloToJson(const std::string& instanceId, uint64_t epoch, uint64_t version) {
    Json::Value j;
    j["instance"] = instanceId;
    j["epoch"]    = (Json::UInt64)epoch;
    j["version"]  = (Json::UInt64)version;
    return dump(j);
}

bool helloFromJson(const std::string& s, std::string& instanceId, uint64_t& epoch, uint64_t& version) {
    Json::Value j;
    if(!load(s, j) || !j.isObject()) return false;
    if(!j["instance"].isString() || j["instance"].asString().empty()
       || !j["epoch"].isUInt64() || !j["version"].isUInt64()) return false;
    instanceId = j["instance"].asString();
    epoch      = j["epoch"].asUInt64();
    version    = j["version"].asUInt64();
    return true;
}

std::string readyToJson(uint64_t epoch, uint64_t version) {
    Json::Value j;
    j["epoch"] = (Json::UInt64)epoch;
    j["version"] = (Json::UInt64)version;
    return dump(j);
}

bool readyFromJson(const std::string& s, uint64_t& epoch, uint64_t& version) {
    Json::Value j;
    if(!load(s, j) || !j.isObject()
       || !j["epoch"].isUInt64() || !j["version"].isUInt64()) return false;
    epoch = j["epoch"].asUInt64();
    version = j["version"].asUInt64();
    return true;
}

std::string errToJson(const std::string& code) {
    Json::Value j;
    j["code"] = code;
    return dump(j);
}

bool errFromJson(const std::string& s, std::string& code) {
    Json::Value j;
    if(!load(s, j) || !j.isObject() || !j["code"].isString()
       || j["code"].asString().empty()) return false;
    code = j["code"].asString();
    return true;
}

std::string deltaToJson(uint64_t epoch, uint64_t prevVer, uint64_t newVer, const std::vector<DeltaOp>& ops) {
    Json::Value j;
    j["epoch"] = (Json::UInt64)epoch;
    j["prev"] = (Json::UInt64)prevVer;
    j["ver"]  = (Json::UInt64)newVer;
    Json::Value arr(Json::arrayValue);
    for(const auto& op : ops) {
        Json::Value o;
        if(op.del) {
            o["del"] = true;
            o["id"]  = op.ruleId;
        } else {
            o["del"]  = false;
            o["rule"] = ruleToNode(op.rule);
        }
        arr.append(o);
    }
    j["ops"] = arr;
    return dump(j);
}

bool deltaFromJson(const std::string& s, uint64_t& epoch, uint64_t& prevVer, uint64_t& newVer,
                   std::vector<DeltaOp>& ops) {
    Json::Value j;
    if(!load(s, j) || !j.isObject()) return false;
    epoch   = j["epoch"].asUInt64();
    prevVer = j["prev"].asUInt64();
    newVer  = j["ver"].asUInt64();
    ops.clear();
    const Json::Value& arr = j["ops"];
    if(!arr.isArray()) return false;
    for(const auto& o : arr) {
        if(!o.isObject() || !o["del"].isBool()) return false;
        DeltaOp op;
        op.del = o["del"].asBool();
        if(op.del) {
            if(!o["id"].isString() || o["id"].asString().empty()) return false;
            op.ruleId = o["id"].asString();
        } else {
            if(!ruleFromNode(o["rule"], op.rule)) return false;
            op.ruleId = op.rule.id;
        }
        ops.push_back(std::move(op));
    }
    return true;
}

std::string ctlPutToJson(const CtlPut& in) {
    Json::Value j;
    j["ip"] = ipStr(in.ip);
    j["action"] = actName(in.action);
    j["ttl_ms"] = (Json::UInt64)in.ttlMs;
    j["reason"] = in.reason;
    return dump(j);
}

bool ctlPutFromJson(const std::string& s, CtlPut& out) {
    Json::Value j;
    if(!load(s, j) || !j.isObject()) return false;
    if(!j["ip"].isString() || !parseCidr(j["ip"].asString(), out.ip)) return false;
    if(!j["action"].isString()) return false;
    const std::string action = j["action"].asString();
    if(action == "allow") out.action = Act::ALLOW;
    else if(action == "deny") out.action = Act::DENY;
    else return false;
    if(!j["ttl_ms"].isUInt64() || !j["reason"].isString()) return false;
    out.ttlMs = j["ttl_ms"].asUInt64();
    out.reason = j["reason"].asString();
    return out.reason.size() <= 256;
}

std::string ctlDelToJson(const std::string& id) {
    Json::Value j;
    j["id"] = id;
    return dump(j);
}

bool ctlDelFromJson(const std::string& s, std::string& id) {
    Json::Value j;
    if(!load(s, j) || !j.isObject() || !j["id"].isString()) return false;
    id = j["id"].asString();
    return !id.empty() && id.size() <= 256;
}

std::string ctlResToJson(const CtlRes& in) {
    Json::Value j;
    j["ok"] = in.ok;
    j["changed"] = in.changed;
    j["version"] = (Json::UInt64)in.version;
    j["id"] = in.id;
    j["error"] = in.error;
    Json::Value rules(Json::arrayValue);
    for(const auto& rule : in.rules) rules.append(ruleToNode(rule));
    j["rules"] = rules;
    return dump(j);
}

bool ctlResFromJson(const std::string& s, CtlRes& out) {
    Json::Value j;
    if(!load(s, j) || !j.isObject()) return false;
    if(!j["ok"].isBool() || !j["changed"].isBool()
       || !j["version"].isUInt64() || !j["id"].isString()
       || !j["error"].isString() || !j["rules"].isArray()) return false;
    out.ok = j["ok"].asBool();
    out.changed = j["changed"].asBool();
    out.version = j["version"].asUInt64();
    out.id = j["id"].asString();
    out.error = j["error"].asString();
    out.rules.clear();
    std::unordered_set<std::string> ids;
    for(const auto& node : j["rules"]) {
        Rule rule;
        if(!ruleFromNode(node, rule) || !ids.insert(rule.id).second) return false;
        out.rules.push_back(std::move(rule));
    }
    return true;
}

} // namespace ipban
} // namespace bronx
