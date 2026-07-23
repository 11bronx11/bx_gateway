// 长期稳定性测试入口（默认 4h，可用 SOAK_DURATION 覆盖）
// 用法：k6 run test/k6/soak.js
//       SOAK_DURATION=8h k6 run test/k6/soak.js
// 注意：别用 K6_DURATION——那是 k6 保留变量,会拿单 VU default 场景盖掉 options.scenarios
// 外部监控：另开终端跑 bash test/api_gw/run_soak.sh 或自行 watch /proc/<pid>/status
import http              from 'k6/http';
import ws                from 'k6/ws';
import { check, sleep }  from 'k6';
import { GW_BASE, ADMIN_BASE, API_PATH, WS_BASE,
         NORMAL_RPS, NORMAL_P99_MS, RATE_PROBE_IP,
         RATE_PROBE_INTERVAL, uniqueRateProbeIp, uniqueWafProbeIp } from './lib/config.js';
import { makeTokens, bearerHeader } from './lib/jwt.js';
import { gwErrors, rateLimited, authRejected,
         wafBlocked, cbOpen, wsFrames, wsReplies } from './lib/metrics.js';
import { readApiRateLimit } from './lib/admin.js';

const DURATION = __ENV.SOAK_DURATION || '4h';

export const options = {
  scenarios: {
    // 主流量：正常 JWT 请求。破坏性限流探针使用独立 XFF 客户端，不能污染它。
    normal_jwt: {
      executor: 'constant-arrival-rate', rate: NORMAL_RPS, timeUnit: '1s',
      duration: DURATION, preAllocatedVUs: 25, exec: 'normalJwt',
    },
    // 鉴权攻击向量（过期/错误secret/alg:none）
    auth_attack: {
      executor: 'constant-arrival-rate', rate: 5, timeUnit: '1s',
      duration: DURATION, preAllocatedVUs: 5, exec: 'authAttack',
    },
    // WAF 组合验证。专题场景覆盖全部规则，这里只保留一次真实拦截，避免持续制造封禁规则。
    waf_probe: {
      executor: 'per-vu-iterations', vus: 1, iterations: 1,
      maxDuration: '30s', exec: 'wafProbe',
    },
    // WebSocket 长连接
    ws_clients: {
      executor: 'constant-vus', vus: 3,
      duration: DURATION, exec: 'wsClient',
    },
    // admin 轮询：持续采 /stats，验证控制面可用且返回结构完整
    admin_poll: {
      executor: 'constant-arrival-rate', rate: 1, timeUnit: '15s',
      duration: DURATION, preAllocatedVUs: 1, exec: 'adminPoll',
    },
    // 限流探针：独立逻辑客户端定期突发，必须先触发 429 再恢复。
    rate_probe: {
      executor: 'constant-arrival-rate', rate: 1, timeUnit: RATE_PROBE_INTERVAL,
      duration: DURATION, preAllocatedVUs: 2, exec: 'rateProbe',
    },
  },
  thresholds: {
    // 每类请求都标记其期望状态，因此全局 http_req_failed 为真实通信或语义失败。
    http_req_failed:                             ['rate==0'],
    'http_req_failed{scenario:normal_jwt}':     ['rate==0'],
    'http_req_duration{scenario:normal_jwt}':   [`p(99)<${NORMAL_P99_MS}`],
    'checks{scenario:normal_jwt}':              ['rate==1'],
    'checks{scenario:auth_attack}':             ['rate==1'],
    'checks{scenario:waf_probe}':               ['rate==1'],
    'checks{scenario:ws_clients}':              ['rate==1'],
    'checks{scenario:admin_poll}':              ['rate==1'],
    'checks{scenario:rate_probe}':              ['rate==1'],
    gw_errors:                                  ['count==0'],
    gw_auth_rejected:                           ['count==0'],
    gw_waf_blocked:                             ['count>0'],
    'gw_rate_limited{probe:rate}':              ['count>0'],
    gw_ws_frames:                               ['count>0'],
    gw_ws_replies:                              ['count>0'],
  },
};

const T   = makeTokens();
const URL = `${GW_BASE}${API_PATH}`;

const WAF_PAYLOADS = [
  "' OR '1'='1", '<script>alert(1)</script>',
  '../../etc/passwd', 'UNION SELECT null--',
];

export function setup() {
  return { rateLimit: readApiRateLimit() };
}

export function normalJwt() {
  const r = http.get(URL, {
    headers: bearerHeader(T.valid),
    responseCallback: http.expectedStatuses(200),
  });
  if (r.status >= 500) gwErrors.add(1);
  check(r, {
    'normal: upstream response is 200': res => res.status === 200,
    'normal: response came from mock upstream': res => {
      try { return JSON.parse(res.body).upstream === 'soak-mock'; }
      catch (_e) { return false; }
    },
  });
}

export function authAttack() {
  for (const [tag, token] of [
    ['expired',    T.expired],
    ['bad_secret', T.badSecret],
    ['alg_none',   T.algNone],
  ]) {
    const r = http.get(URL, {
      headers: bearerHeader(token),
      responseCallback: http.expectedStatuses(401),
    });
    const ok = check(r, { [`auth attack ${tag} → 401`]: res => res.status === 401 });
    if (!ok) authRejected.add(1);
  }
}

export function wafProbe() {
  const q = WAF_PAYLOADS[Math.floor(Math.random() * WAF_PAYLOADS.length)];
  const r = http.get(`${URL}?q=${encodeURIComponent(q)}`, {
    headers: { 'X-Forwarded-For': uniqueWafProbeIp((__VU - 1) * 1000000 + __ITER) },
    responseCallback: http.expectedStatuses(403),
  });
  if (r.status === 403) wafBlocked.add(1);
  check(r, { 'waf probe: injection blocked (403)': res => res.status === 403 });
}

export function wsClient() {
  let received = 0;
  const res = ws.connect(`${WS_BASE}/api/ws`, { headers: bearerHeader(T.valid) }, (socket) => {
    socket.on('open', () => { socket.send('ping'); wsFrames.add(1); });
    socket.on('message', () => {
      received++;
      wsFrames.add(1);
      wsReplies.add(1);
      socket.close();
    });
    socket.on('error', () => { check(null, { 'ws no error': () => false }); });
    socket.setTimeout(() => socket.close(), 20000);
  });
  check(res, { 'ws 101': r => r.status === 101 });
  check({ received }, { 'ws ping receives echo': s => s.received > 0 });
  sleep(2);
}

export function adminPoll() {
  const r = http.get(`${ADMIN_BASE}/stats`, { responseCallback: http.expectedStatuses(200) });
  check(r, {
    'admin /stats 200': res => res.status === 200,
    'admin /stats has circuit counter': res => {
      try { return typeof JSON.parse(res.body).circuit_open === 'number'; }
      catch (_e) { return false; }
    },
  });
  if (r.status === 200) {
    try { cbOpen.add(JSON.parse(r.body).circuit_open || 0); } catch (_e) { /* ignore */ }
  }
}

export function rateProbe(data) {
  const headers = Object.assign({
    'X-Forwarded-For': uniqueRateProbeIp((__VU - 1) * 1000000 + __ITER),
  }, bearerHeader(T.valid));
  const requests = [];
  const burst = Math.ceil(data.rateLimit.capacity * 1.5);
  for (let i = 0; i < burst; i++) {
    requests.push(['GET', URL, null, {
      headers,
      tags: { probe: 'rate' },
      responseCallback: http.expectedStatuses(200, 429),
    }]);
  }
  let throttled = 0;
  let unexpected = 0;
  for (const r of http.batch(requests)) {
    if (r.status === 429) {
      throttled++;
      rateLimited.add(1, { probe: 'rate' });
    } else if (r.status !== 200) {
      unexpected++;
      if (r.status >= 500) gwErrors.add(1);
    }
  }
  check({ throttled, unexpected }, {
    'rate probe: burst triggers 429': s => s.throttled > 0,
    'rate probe: burst has only 200 or 429': s => s.unexpected === 0,
  });

  sleep(1);
  const r = http.get(URL, {
    headers,
    tags: { probe: 'rate' },
    responseCallback: http.expectedStatuses(200),
  });
  check(r, { 'rate probe: recovery returns 200': res => res.status === 200 });
}

// k6 要求至少一个 default export；soak 的入口是各具名函数，default 留空
export default function () {}
