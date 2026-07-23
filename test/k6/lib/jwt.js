import encoding from 'k6/encoding';
import crypto   from 'k6/crypto';
import { JWT_SECRET, JWT_ISSUER } from './config.js';

// HS256 JWT 生成，payload 合并传入的 claims
export function makeJwt(claims, secret = JWT_SECRET) {
  const now = Math.floor(Date.now() / 1000);
  const full = Object.assign({ iss: JWT_ISSUER, iat: now }, claims);
  const h = encoding.b64encode(JSON.stringify({ alg: 'HS256', typ: 'JWT' }), 'rawurl');
  const b = encoding.b64encode(JSON.stringify(full), 'rawurl');
  const sig = crypto.hmac('sha256', secret, `${h}.${b}`, 'base64rawurl');
  return `${h}.${b}.${sig}`;
}

// 预制各类 token，在 init 阶段调用（VU 间共享）
export function makeTokens() {
  const now = Math.floor(Date.now() / 1000);
  return {
    // 正常 token，1h 有效
    valid:      makeJwt({ sub: 'user1', exp: now + 3600, scope: 'read write' }),
    // 已过期
    expired:    makeJwt({ sub: 'user1', exp: now - 3600 }),
    // 错误 secret
    badSecret:  makeJwt({ sub: 'user1', exp: now + 3600 }, 'wrong-secret'),
    // alg:none 攻击：手拼，不带签名
    algNone: (() => {
      const h = encoding.b64encode(JSON.stringify({ alg: 'none', typ: 'JWT' }), 'rawurl');
      const b = encoding.b64encode(JSON.stringify({ sub: 'admin', exp: now + 3600 }), 'rawurl');
      return `${h}.${b}.`;
    })(),
    // 裸 sub 无 exp（取决于网关是否要求 exp）
    noExp: makeJwt({ sub: 'user1' }),
  };
}

export function bearerHeader(token) {
  return { Authorization: `Bearer ${token}` };
}
