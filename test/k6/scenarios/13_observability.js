// 可观测性全覆盖测试
// 验证 /stats /routes /healthz（网关 admin）+ hub /healthz /metrics（Prometheus）
// 并确认流量后关键计数器会增长
import http      from 'k6/http';
import { check, sleep } from 'k6';
import {
  GW_BASE, ADMIN_BASE, API_PATH,
  HUB_BASE,
} from '../lib/config.js';
import { makeTokens, bearerHeader } from '../lib/jwt.js';
import { gwErrors }                 from '../lib/metrics.js';

export const options = {
  scenarios: {
    // Phase 1：先快照初始 stats
    snapshot_before: {
      executor:   'per-vu-iterations', vus: 1, iterations: 1,
      maxDuration: '30s', exec: 'snapshotBefore', startTime: '0s',
    },
    // Phase 2：打一批真实流量
    traffic:  {
      executor:   'constant-arrival-rate',
      rate: 30, timeUnit: '1s',
      duration:   '20s',
      preAllocatedVUs: 5,
      exec:        'sendTraffic',
      startTime:   '5s',
    },
    // Phase 3：验 stats 已增长 + 全接口健康
    verify_after: {
      executor:   'per-vu-iterations', vus: 1, iterations: 1,
      maxDuration: '30s', exec: 'verifyAfter', startTime: '30s',
    },
  },
  thresholds: {
    http_req_failed:                       ['rate==0'],
    'checks{scenario:snapshot_before}': ['rate==1'],
    'checks{scenario:traffic}':         ['rate==1'],
    'checks{scenario:verify_after}':    ['rate==1'],
    gw_errors:                          ['count==0'],
  },
};

const T    = makeTokens();
const AUTH = bearerHeader(T.valid);

// ── 基础字段断言 ──────────────────────────────────────────────
function checkStats(r, tag) {
  const ok = check(r, {
    [`${tag} stats: 200`]:          res => res.status === 200,
    [`${tag} stats: json`]:         res => {
      try { JSON.parse(res.body); return true; } catch (_e) { return false; }
    },
    [`${tag} stats: has requests`]: res => {
      try {
        const j = JSON.parse(res.body);
        return typeof j.requests === 'number';
      } catch (_e) { return false; }
    },
    [`${tag} stats: has upstream fields`]: res => {
      try {
        const j = JSON.parse(res.body);
        return typeof j.upstream_ok === 'number'
            && typeof j.upstream_fail === 'number';
      } catch (_e) { return false; }
    },
  });
  if (!ok) gwErrors.add(1);
  return r.status === 200 ? JSON.parse(r.body) : null;
}

function checkRoutes(r) {
  check(r, {
    'routes: 200': res => res.status === 200,
    'routes: json array or object': res => {
      try { const j = JSON.parse(res.body); return j !== null; } catch (_e) { return false; }
    },
  });
}

function checkHealthz(r, label) {
  check(r, {
    [`${label} healthz: 200`]: res => res.status === 200,
  });
}

function checkHubMetrics(r) {
  check(r, {
    'hub metrics: 200':             res => res.status === 200,
    'hub metrics: prometheus text': res => {
      // Prometheus text format 以 "# HELP" 或 metric_name 开头
      const body = res.body || '';
      return body.includes('# HELP') || body.includes('ipban') || body.includes('hub_');
    },
    'hub metrics: has ipban_rules field': res => {
      return (res.body || '').includes('ipban_rules');
    },
  });
}

// ── 执行函数 ──────────────────────────────────────────────────

export function setup() {
  const r = http.get(`${ADMIN_BASE}/stats`, { responseCallback: http.expectedStatuses(200) });
  const stats = checkStats(r, 'setup');
  if (stats === null || typeof stats.requests !== 'number') {
    throw new Error('cannot capture initial gateway stats');
  }
  return { requests: stats.requests };
}

export function snapshotBefore() {
  // 只做一次：验所有接口能通。
  checkStats  (http.get(`${ADMIN_BASE}/stats`, { responseCallback: http.expectedStatuses(200) }), 'before');
  checkRoutes (http.get(`${ADMIN_BASE}/routes`));
  checkHealthz(http.get(`${GW_BASE}/healthz`), 'gw');

  if (__ENV.K6_HUB_PORT) {
    checkHealthz(http.get(`${HUB_BASE}/healthz`),          'hub');
    checkHubMetrics(http.get(`${HUB_BASE}/metrics`));
  }
}

export function sendTraffic() {
  // 发各种请求，让计数器动起来
  const paths = [API_PATH, '/healthz', `${API_PATH}?v=obs`];
  const p = paths[__ITER % paths.length];
  const r = http.get(`${GW_BASE}${p}`, {
    headers: AUTH,
    responseCallback: http.expectedStatuses(200),
  });
  if (r.status >= 500) gwErrors.add(1);
  check(r, { 'traffic request: 200': res => res.status === 200 });
}

export function verifyAfter(data) {
  sleep(1); // 等 stats 刷新

  // 1. /stats —— requests 必须增长（流量打进来了）
  const sr = http.get(`${ADMIN_BASE}/stats`, { responseCallback: http.expectedStatuses(200) });
  const stats = checkStats(sr, 'after');
  if (stats !== null) {
    check(stats, {
      'stats after traffic: requests increased': s =>
        data && s.requests > data.requests,
    });

    // 限流计数存在（即使值为 0）
    check(stats, {
      'stats: rate_limited counter present': s => typeof s.rate_limited === 'number',
    });

    check(stats, {
      'stats: per-route counters field': s => s.routes !== null && typeof s.routes === 'object',
    });
  }

  // 2. /routes —— 路由表非空
  const rr = http.get(`${ADMIN_BASE}/routes`);
  checkRoutes(rr);
  check(rr, {
    'routes: non-empty': res => {
      try {
        const j = JSON.parse(res.body);
        return Array.isArray(j) ? j.length > 0 : Object.keys(j).length > 0;
      } catch (_e) { return false; }
    },
  });

  // 3. /healthz
  checkHealthz(http.get(`${GW_BASE}/healthz`), 'gw-after');

  // 4. hub（可选，K6_HUB_PORT 非空时才跑）
  if (__ENV.K6_HUB_PORT) {
    checkHealthz(http.get(`${HUB_BASE}/healthz`), 'hub-after');
    checkHubMetrics(http.get(`${HUB_BASE}/metrics`));

    // hub /metrics 至少保留可读的 Prometheus 内容
    const hm = http.get(`${HUB_BASE}/metrics`);
    check(hm, {
      'hub metrics after traffic: has content': res => (res.body || '').length > 50,
    });
  }
}

export default function () {}
