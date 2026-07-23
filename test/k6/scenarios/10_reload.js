// 热重载基础测试：reload 期间流量连续性 + 配置生效验证
import http              from 'k6/http';
import { check, sleep }  from 'k6';
import { GW_BASE, ADMIN_BASE, API_PATH } from '../lib/config.js';
import { makeTokens, bearerHeader }      from '../lib/jwt.js';
import { gwErrors }                      from '../lib/metrics.js';

export const options = {
  scenarios: {
    // 背景流量：全程持续
    background: {
      executor: 'constant-arrival-rate', rate: 30, timeUnit: '1s',
      duration: '3m', preAllocatedVUs: 15, exec: 'backgroundTraffic',
    },
    // reload 触发器：每20s 触发一次，共触发6次
    reloader: {
      executor: 'constant-arrival-rate', rate: 1, timeUnit: '20s',
      duration: '3m', preAllocatedVUs: 1, exec: 'triggerReload',
    },
  },
  thresholds: {
    http_req_failed:                            ['rate==0'],
    'http_req_failed{scenario:background}':     ['rate==0'],
    'checks{scenario:background}':              ['rate==1'],
    'checks{scenario:reloader}':                ['rate==1'],
    'http_req_duration{scenario:background}':   ['p(99)<2000'],
    gw_errors:                                  ['count==0'],
  },
};

const URL        = `${GW_BASE}${API_PATH}`;
const RELOAD_URL = `${ADMIN_BASE}/reload`;
const STATS_URL  = `${ADMIN_BASE}/stats`;
const T          = makeTokens();
const AUTH       = bearerHeader(T.valid);

export function backgroundTraffic() {
  const r = http.get(URL, {
    headers: AUTH,
    responseCallback: http.expectedStatuses(200),
  });
  if (r.status >= 500) gwErrors.add(1);
  check(r, {
    'reload traffic: response is 200': res => res.status === 200,
  });
}

export function triggerReload() {
  // 记录 reload 前的 stats 快照
  const before = http.get(STATS_URL, { responseCallback: http.expectedStatuses(200) });
  const statsBefore = before.status === 200 ? JSON.parse(before.body) : {};
  const routesBefore = http.get(`${ADMIN_BASE}/routes`, { responseCallback: http.expectedStatuses(200) });

  // 触发 reload
  const r = http.post(RELOAD_URL, null, {
    timeout: '5s',
    responseCallback: http.expectedStatuses(200),
  });
  check(r, { 'POST /reload 200': res => res.status === 200 });

  // reload 后立刻验 admin 仍可达
  sleep(0.5);
  const health = http.get(`${ADMIN_BASE}/healthz`, { responseCallback: http.expectedStatuses(200) });
  check(health, { '/healthz alive after reload': res => res.status === 200 });

  // stats 仍返回合法 JSON
  const after = http.get(STATS_URL, { responseCallback: http.expectedStatuses(200) });
  check(after, {
    '/stats valid after reload': res => {
      if (res.status !== 200) return false;
      try { JSON.parse(res.body); return true; }
      catch (_e) { return false; }
    },
  });

  // 请求计数只能增不能重置（reload 不清指标）
  if (before.status === 200 && after.status === 200) {
    const b = statsBefore.requests || 0;
    const a = JSON.parse(after.body).requests || 0;
    check({ before: b, after: a }, {
      'reload: request counter not reset': s => s.after >= s.before,
    });
  }

  const routesAfter = http.get(`${ADMIN_BASE}/routes`, { responseCallback: http.expectedStatuses(200) });
  check({ before: routesBefore, after: routesAfter }, {
    'reload: route configuration remains readable': pair => {
      if (pair.before.status !== 200 || pair.after.status !== 200) return false;
      try {
        const beforeRoutes = JSON.parse(pair.before.body).routes || [];
        const afterRoutes = JSON.parse(pair.after.body).routes || [];
        return beforeRoutes.some(route => route.path === '/api')
          && afterRoutes.some(route => route.path === '/api');
      } catch (_e) { return false; }
    },
  });
}

export default function () {}
