// 快速冒烟（~1min）：验核心链路没有明显断裂
// 用法：k6 run test/k6/smoke.js
import http              from 'k6/http';
import { check }         from 'k6';
import { GW_BASE, ADMIN_BASE, API_PATH } from './lib/config.js';
import { makeTokens, bearerHeader }      from './lib/jwt.js';

export const options = {
  scenarios: {
    smoke: { executor: 'constant-arrival-rate', rate: 5, timeUnit: '1s',
             duration: '1m', preAllocatedVUs: 5 },
  },
  thresholds: {
    http_req_failed:   ['rate==0'],
    http_req_duration: ['p(99)<2000'],
    checks:            ['rate==1'],
  },
};

const T   = makeTokens();
const URL = `${GW_BASE}${API_PATH}`;

export default function () {
  // 正常鉴权
  check(http.get(URL, {
    headers: bearerHeader(T.valid),
    responseCallback: http.expectedStatuses(200),
  }), {
    'smoke: valid jwt 200': r => r.status === 200,
  });

  // 过期 token 拒绝
  check(http.get(URL, {
    headers: bearerHeader(T.expired),
    responseCallback: http.expectedStatuses(401),
  }), {
    'smoke: expired → 401': r => r.status === 401,
  });

  // healthz 可达
  check(http.get(`${GW_BASE}/healthz`, { responseCallback: http.expectedStatuses(200) }), {
    'smoke: /healthz 200': r => r.status === 200,
  });

  // admin stats 可达
  check(http.get(`${ADMIN_BASE}/stats`, { responseCallback: http.expectedStatuses(200) }), {
    'smoke: /stats 200': r => r.status === 200,
  });

  // 未知路由 404
  check(http.get(`${GW_BASE}/no-such-route`, { responseCallback: http.expectedStatuses(404) }), {
    'smoke: unknown path 404': r => r.status === 404,
  });
}
