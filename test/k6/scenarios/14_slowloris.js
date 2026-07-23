// HTTP 慢速攻击 + 网关韧性
//
// k6 没有 raw socket，无法模拟真正的 slowloris（发半截请求头后停止发送）。
// 真实 slowloris 测试请用：
//   slowhttptest -c 200 -H -i 15 -r 200 -t GET -u http://127.0.0.1:8090/api/users
//
// 本脚本测的是等效的协程槽耗尽场景：
//   attacker VU 把大量请求打到"永远慢"的上游端点，让网关协程全部卡在 proxy wait；
//   与此同时验证正常请求依然能在合理延迟内完成。
//
// 前提：mock_rich.py /_soak/control 支持 slow_probability / slow_delay_ms，
//       K6_SLOW_API_PATH 只路由到这个受控慢上游。
import http              from 'k6/http';
import { check, sleep }  from 'k6';
import { GW_BASE, MOCK_BASE, API_PATH, ADMIN_BASE } from '../lib/config.js';
import { makeTokens, bearerHeader }                 from '../lib/jwt.js';
import { gwErrors, slowHeld }                       from '../lib/metrics.js';

// 攻击 VU 数——可通过环境变量调高
const ATTACKER_VUS = parseInt(__ENV.K6_SL_VUS || '30');
const MOCK_DELAY   = parseInt(__ENV.K6_SL_MOCK_DELAY_MS || '25000'); // ms
const SLOW_API_PATH = __ENV.K6_SLOW_API_PATH || '';
const NORMAL_API_PATH = __ENV.K6_NORMAL_API_PATH || API_PATH;
const MIN_HOLD_MS = parseInt(__ENV.K6_SLOW_MIN_HOLD_MS || '1000');

export const options = {
  scenarios: {
    // 攻击方：同时打 ATTACKER_VUS 个"慢请求"，占满协程槽
    slow_hold: {
      executor:    'constant-vus',
      vus:         ATTACKER_VUS,
      duration:    '60s',
      exec:        'slowAttack',
      startTime:   '3s',  // 等 setup 完成
    },
    // 正常客户端：攻击期间以恒速 20 req/s 发请求，验可用性
    normal_traffic: {
      executor:    'constant-arrival-rate',
      rate:        20, timeUnit: '1s',
      duration:    '60s',
      preAllocatedVUs: 8,
      exec:        'normalTraffic',
      startTime:   '5s',
    },
    // 攻击后健康检查
    post_attack: {
      executor:    'per-vu-iterations', vus: 1, iterations: 5,
      maxDuration: '30s', exec: 'postAttack',
      startTime:   '70s',
    },
  },
  thresholds: {
    // 压力下正常流量必须继续可用，攻击后的控制面必须完整恢复。
    'checks{exec:slowAttack}':                   ['rate==1'],
    'checks{exec:normalTraffic}':                ['rate==1'],
    'http_req_duration{exec:normalTraffic}':     ['p(95)<5000'],
    'checks{exec:postAttack}':                   ['rate==1'],
    gw_slow_held:                                ['count>0'],
    gw_errors: ['count==0'],
  },
  setupTimeout: '30s',
};

const T    = makeTokens();
const AUTH = bearerHeader(T.valid);

// setup：把 mock 配成高延迟
export function setup() {
  if (!SLOW_API_PATH) {
    throw new Error('K6_SLOW_API_PATH is required and must route only to the controlled slow upstream');
  }
  const r = http.post(
    `${MOCK_BASE}/_soak/control`,
    JSON.stringify({ slow_probability: 1, slow_delay_ms: MOCK_DELAY }),
    { headers: { 'Content-Type': 'application/json' } },
  );
  if (r.status !== 200) throw new Error(`cannot enable slow mock, status=${r.status}`);
  try {
    const cfg = JSON.parse(r.body).config || {};
    if (cfg.slow_probability !== 1 || cfg.slow_delay_ms !== MOCK_DELAY) {
      throw new Error('mock did not retain requested slow configuration');
    }
  } catch (error) {
    throw new Error(`cannot verify slow mock configuration: ${error}`);
  }
  sleep(1);
}

// teardown：恢复 mock 正常速度
export function teardown() {
  http.post(
    `${MOCK_BASE}/_soak/control`,
    JSON.stringify({ slow_probability: 0, slow_delay_ms: 0 }),
    { headers: { 'Content-Type': 'application/json' } },
  );
}

// 攻击 VU：发送请求到慢 mock，等网关超时或上游返回
export function slowAttack() {
  // 超时略高于 mock 延迟，让连接尽量挂着
  const started = Date.now();
  const r = http.get(`${GW_BASE}${SLOW_API_PATH}`, {
    headers: AUTH,
    timeout: `${MOCK_DELAY + 5000}ms`,
    responseCallback: http.expectedStatuses(200, 502, 503, 504),
  });
  const elapsed = Date.now() - started;
  if (elapsed >= MIN_HOLD_MS) slowHeld.add(1);
  check(r, {
    'slow attack: gateway returned an expected slow-path outcome': res =>
      res.status === 200 || res.status === 502 || res.status === 503 || res.status === 504,
  });
  // 503 是已经打开的熔断器的快速保护；其余响应必须实际经历受控慢上游等待。
  if (r.status !== 503) {
    check({ elapsed }, {
      'slow attack: request held by controlled slow upstream': s => s.elapsed >= MIN_HOLD_MS,
    });
  }
  sleep(0.1); // 短暂重置再打下一个
}

// 正常 VU：快速请求，验可用性
export function normalTraffic() {
  const r = http.get(`${GW_BASE}${NORMAL_API_PATH}`, {
    headers: AUTH,
    timeout: '6s',
    tags: { exec: 'normalTraffic' },
    responseCallback: http.expectedStatuses(200),
  });
  if (r.status >= 500) gwErrors.add(1);
  check(r, {
    'normal req under attack: served with 200': res => res.status === 200,
  });
}

// 攻击后：网关应完全恢复
export function postAttack() {
  const h = http.get(`${GW_BASE}/healthz`, {
    timeout: '3s',
    tags: { exec: 'postAttack' },
    responseCallback: http.expectedStatuses(200),
  });
  check(h, { 'post-attack healthz: 200': res => res.status === 200 });

  // /stats 仍可读
  const s = http.get(`${ADMIN_BASE}/stats`, {
    timeout: '3s',
    responseCallback: http.expectedStatuses(200),
  });
  check(s, { 'post-attack stats: 200': res => res.status === 200 });
  sleep(1);
}

export default function () {}
