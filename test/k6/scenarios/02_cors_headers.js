import http      from 'k6/http';
import { check } from 'k6';
import { GW_BASE, API_PATH } from '../lib/config.js';
import { makeTokens, bearerHeader } from '../lib/jwt.js';

export const options = {
  scenarios: {
    cors: { executor: 'constant-arrival-rate', rate: 5, timeUnit: '1s', duration: '2m', preAllocatedVUs: 5 },
  },
  thresholds: { checks: ['rate==1'], http_req_failed: ['rate==0'] },
};

const URL = `${GW_BASE}${API_PATH}`;
const T   = makeTokens();

export default function () {
  // CORS preflight（OPTIONS）：合法 origin + short_circuit_preflight=true → 204 短路
  // 且回带正确的 Allow-Origin(=请求 origin) 和 Allow-Methods 头
  {
    const r = http.options(URL, null, {
      headers: {
        Origin: 'https://legit.example.com',
        'Access-Control-Request-Method': 'GET',
      },
      responseCallback: http.expectedStatuses(204),
    });
    check(r, {
      'cors preflight → 204 short-circuit': res => res.status === 204,
      'cors allow-origin echoes legit origin': res =>
        res.headers['Access-Control-Allow-Origin'] === 'https://legit.example.com',
      'cors preflight has allow-methods': res =>
        res.headers['Access-Control-Allow-Methods'] !== undefined,
    });
  }

  // 正常请求的 CORS 头 + 安全头
  {
    const r = http.get(URL, {
      headers: Object.assign(bearerHeader(T.valid), { Origin: 'https://legit.example.com' }),
      responseCallback: http.expectedStatuses(200),
    });
    check(r, {
      'x-content-type-options: nosniff': res =>
        (res.headers['X-Content-Type-Options'] || '').toLowerCase() === 'nosniff',
      'x-frame-options present': res => res.headers['X-Frame-Options'] !== undefined,
      'no server version leak':  res => !res.headers['Server'] || res.headers['Server'] === '',
    });
  }

  // 恶意 Origin 不应被反射
  {
    const r = http.get(URL, {
      headers: Object.assign(bearerHeader(T.valid), { Origin: 'https://evil.attacker.com' }),
      responseCallback: http.expectedStatuses(200),
    });
    check(r, {
      // 不在白名单 → pick_origin 返回空 → 根本不贴 Allow-Origin 头（比"不等于反射"更严）
      'evil origin: no allow-origin header at all': res =>
        res.headers['Access-Control-Allow-Origin'] === undefined,
    });
  }
}
