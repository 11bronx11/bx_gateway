#include "rule.h"
#include <cstring>

namespace bronx {
namespace ipban {

const char* srcName(Src s) {
    switch(s) {
        case Src::WAF:    return "waf";
        case Src::RATE:   return "rate";
        case Src::BIZ:    return "biz";
        case Src::ADMIN:  return "admin";
        case Src::STATIC: return "static";
        case Src::EMERG:  return "emerg";
    }
    return "?";
}

const char* actName(Act a) {
    return a == Act::DENY ? "deny" : "allow";
}

Src srcFromName(const std::string& s) {
    if(s == "waf")    return Src::WAF;
    if(s == "rate")   return Src::RATE;
    if(s == "biz")    return Src::BIZ;
    if(s == "admin")  return Src::ADMIN;
    if(s == "static") return Src::STATIC;
    if(s == "emerg")  return Src::EMERG;
    return Src::RATE;
}

Act actFromName(const std::string& s) {
    return s == "allow" ? Act::ALLOW : Act::DENY;
}

} // namespace ipban
} // namespace bronx
