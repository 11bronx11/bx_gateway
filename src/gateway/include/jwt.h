#pragma once

// 验 Authorization Bearer token, 按配的算法验签, 过期/签名不对/issuer 或 aud 不符都拒。
// alg 锁死单一算法, token 不能自己挑, alg=none 直接拒, 这俩是常见绕过。
// 验过了把 sub scope roles 写进 ctx, 后面限流和授权透传要用。

#include "middlewares/stubs.h"
#include <string>
#include <vector>

namespace bronx {
namespace gateway {

struct JwtKey {
    std::string kid;
    std::string algo;
    std::string pubKey;
};

struct JwtAuthConfig {
    bool        enabled = false;
    std::string algo    = "HS256";  // HS256 | RS256 | ES256
    std::string secret;             // HS256 用的对称密钥
    std::string pubKey;             // RS256/ES256 用的 PEM 公钥
    std::string issuer;             // 空=不验
    std::string audience;           // 空=不验
    uint32_t    leewaySec = 0;      // 时钟容忍, 扛机器间漂移
    std::vector<JwtKey> keys;
};

class JwtAuthenticator : public Authenticator {
public:
    explicit JwtAuthenticator(const JwtAuthConfig& cfg);
    // 返回 Ok 放行, 其余短路 401。
    // 成功时往 ctx 写 jwt_sub jwt_scopes jwt_roles。
    AuthErr authenticate(ReqCtx& ctx) override;

private:
    JwtAuthConfig m_cfg;
};

} // namespace gateway
} // namespace bronx
