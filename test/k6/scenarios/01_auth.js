import http         from 'k6/http';
import { check }    from 'k6';
import { GW_BASE, API_PATH, API_KEY } from '../lib/config.js';
import { makeTokens, bearerHeader }   from '../lib/jwt.js';
import { authRejected }               from '../lib/metrics.js';

export const options = {
  scenarios: {
    auth: { executor: 'constant-arrival-rate', rate: 20, timeUnit: '1s', duration: '2m', preAllocatedVUs: 10 },
  },
  thresholds: {
    'checks{scenario:auth}': ['rate==1'],
    http_req_failed:         ['rate==0'],
    gw_auth_rejected:        ['count==0'],
  },
};

const URL = `${GW_BASE}${API_PATH}`;

// init 阶段生成，所有 VU 共享
const T = makeTokens();

export default function () {
  // 正常 JWT 必须穿过网关并得到上游成功响应。
  {
    const r = http.get(URL, {
      headers: bearerHeader(T.valid),
      responseCallback: http.expectedStatuses(200),
    });
    const ok = check(r, { 'jwt valid → 200': res => res.status === 200 });
    if (!ok) authRejected.add(1);
  }

  // 过期 → 401
  {
    const r = http.get(URL, {
      headers: bearerHeader(T.expired),
      responseCallback: http.expectedStatuses(401),
    });
    const ok = check(r, { 'jwt expired → 401': res => res.status === 401 });
    if (!ok) authRejected.add(1);
  }

  // 错误 secret → 401
  {
    const r = http.get(URL, {
      headers: bearerHeader(T.badSecret),
      responseCallback: http.expectedStatuses(401),
    });
    check(r, { 'jwt bad secret → 401': res => res.status === 401 });
  }

  // alg:none 攻击 → 401
  {
    const r = http.get(URL, {
      headers: bearerHeader(T.algNone),
      responseCallback: http.expectedStatuses(401),
    });
    check(r, { 'alg:none blocked → 401': res => res.status === 401 });
  }

  // 无鉴权头 → 401
  {
    const r = http.get(URL, { responseCallback: http.expectedStatuses(401) });
    check(r, { 'no auth → 401': res => res.status === 401 });
  }

  // API Key（仅当配置了 K6_API_KEY 时生效）
  if (API_KEY) {
    const r = http.get(URL, {
      headers: { 'X-Api-Key': API_KEY },
      responseCallback: http.expectedStatuses(200),
    });
    check(r, { 'api_key valid → 200': res => res.status === 200 });
  }
}
