#include "jwt.h"
#include "ctx.h"
#include "http_msg.h"
#include <jwt-cpp/jwt.h>
#include <sstream>
#include <vector>
#include <strings.h>

namespace bronx {
namespace gateway {

JwtAuthenticator::JwtAuthenticator(const JwtAuthConfig& cfg) : m_cfg(cfg) {}

// 空格分隔的 scope 串切成一个个 token
static void split_ws(const std::string& s, std::vector<std::string>& out) {
    std::istringstream is(s);
    std::string tok;
    while(is >> tok) out.push_back(tok);
}

AuthErr JwtAuthenticator::authenticate(ReqCtx& ctx) {
    bool asym = (m_cfg.algo == "RS256" || m_cfg.algo == "ES256");
    if(!asym && (!m_cfg.keys.empty() || m_cfg.secret.empty())) return AuthErr::BadSig;
    if(asym && m_cfg.keys.empty() && m_cfg.pubKey.empty()) return AuthErr::BadSig;
    if(asym && !m_cfg.keys.empty() && !m_cfg.pubKey.empty()) return AuthErr::BadSig;

    auto req = ctx.request();
    if(!req) return AuthErr::NoToken;

    std::string auth = req->getHeader("Authorization");
    // scheme 按 RFC 6750 大小写不敏感, bearer 也认
    if(auth.size() < 8 || strncasecmp(auth.c_str(), "Bearer ", 7) != 0) return AuthErr::NoToken;
    std::string token = auth.substr(7);
    if(token.empty()) return AuthErr::NoToken;

    try {
        auto decoded = jwt::decode(token);

        // alg 必须跟配的一样, token 不能自己挑, alg=none 一并挡
        std::string alg = decoded.get_algorithm();
        if(alg == "none" || alg != m_cfg.algo) return AuthErr::BadAlg;

        std::string pubKey = m_cfg.pubKey;
        if(!m_cfg.keys.empty()) {
            if(!decoded.has_key_id()) return AuthErr::BadSig;
            std::string kid = decoded.get_key_id();
            const JwtKey* found = nullptr;
            for(const auto& key : m_cfg.keys) {
                if(key.kid == kid) { found = &key; break; }
            }
            if(!found || found->algo != m_cfg.algo || found->pubKey.empty()) {
                return AuthErr::BadSig;
            }
            pubKey = found->pubKey;
        }

        if(!decoded.has_expires_at()) return AuthErr::BadToken;
        try { (void)decoded.get_expires_at(); }
        catch(...) { return AuthErr::BadToken; }

        // issuer 和 aud 单独查, 失败口径更清楚
        std::error_code ec;
        auto verify_with = [&](auto a) {
            auto v = jwt::verify().allow_algorithm(a);
            if(m_cfg.leewaySec) v.leeway(m_cfg.leewaySec);
            v.verify(decoded, ec);
        };
        if(m_cfg.algo == "HS256")      verify_with(jwt::algorithm::hs256{m_cfg.secret});
        else if(m_cfg.algo == "RS256") verify_with(jwt::algorithm::rs256{pubKey, "", "", ""});
        else if(m_cfg.algo == "ES256") verify_with(jwt::algorithm::es256{pubKey, "", "", ""});
        else return AuthErr::BadAlg;

        if(ec) {
            if(ec == jwt::error::token_verification_error::token_expired)
                return AuthErr::Expired;
            return AuthErr::BadSig;
        }

        // issuer
        if(!m_cfg.issuer.empty()) {
            if(!decoded.has_issuer() || decoded.get_issuer() != m_cfg.issuer)
                return AuthErr::BadIss;
        }
        // audience, aud 可能是串也可能是数组, get_audience 统一成集合
        if(!m_cfg.audience.empty()) {
            if(!decoded.has_audience()) return AuthErr::BadAud;
            auto auds = decoded.get_audience();
            if(auds.find(m_cfg.audience) == auds.end()) return AuthErr::BadAud;
        }

        // 抽身份和权限声明给下游用。sub 可能不是串(有些 IdP 发数字),
        // 抽不出来就留空, 别让它冒到外层 catch 把已验过的 token 误判成烂 token
        std::string sub;
        if(decoded.has_payload_claim("sub")) {
            try { sub = decoded.get_payload_claim("sub").as_string(); }
            catch(...) {}
        }
        ctx.setAttr("jwt_sub", sub);

        std::vector<std::string> scopes;
        if(decoded.has_payload_claim("scope")) {
            try { split_ws(decoded.get_payload_claim("scope").as_string(), scopes); }
            catch(...) {}
        }
        ctx.setAttr("jwt_scopes", scopes);
        ctx.setAttr("auth_scopes", scopes);

        std::vector<std::string> roles;
        if(decoded.has_payload_claim("roles")) {
            try {
                for(const auto& e : decoded.get_payload_claim("roles").as_array())
                    if(e.is<std::string>()) roles.push_back(e.get<std::string>());
            } catch(...) {}
        }
        ctx.setAttr("jwt_roles", roles);

        return AuthErr::Ok;
    } catch(...) {
        return AuthErr::BadToken;
    }
}

} // namespace gateway
} // namespace bronx
