// 限流增强测试
// 从 admin /routes 读取实际 capacity、refill_per_sec 和 key，拒绝与 YAML 漂移的固定参数。
//
// 每个阶段使用独立可信 XFF，避免限流举报形成的临时封禁污染其他判据。
import http              from 'k6/http';
import { check, sleep }  from 'k6';
import { GW_BASE, API_PATH } from '../lib/config.js';
import { makeTokens, bearerHeader } from '../lib/jwt.js';
import { rateLimited }   from '../lib/metrics.js';
import { readApiRateLimit } from '../lib/admin.js';

const MAX_BURST = 4096;

export const options = {
  batch: MAX_BURST,
  batchPerHost: MAX_BURST,
  scenarios: {
    // A: 串行突发，验容量上限
    burst_cap: {
      executor: 'per-vu-iterations', vus: 1, iterations: 1,
      maxDuration: '2m', exec: 'testBurstCapacity',
    },
    // B: 恢复速率，验令牌桶补充速率
    refill_rate: {
      executor: 'per-vu-iterations', vus: 1, iterations: 1,
      maxDuration: '2m', startTime: '45s', exec: 'testRefillRate',
    },
    // C: 稳态低速，验低于 refill 速率不触发限流
    steady_below_limit: {
      executor: 'constant-vus', vus: 1, duration: '30s',
      startTime: '2m', exec: 'testSteady',
    },
    // D: 路由隔离，/api 限流不影响 /healthz
    route_isolation: {
      executor: 'constant-arrival-rate', rate: 150, timeUnit: '1s',
      duration: '15s', preAllocatedVUs: 30,
      startTime: '3m30s', exec: 'testRouteIsolation',
    },
  },
  thresholds: {
    http_req_failed:                              ['rate==0'],
    'gw_rate_limited':                           ['count>0'],
    'checks{scenario:burst_cap}':                ['rate==1'],
    'checks{scenario:refill_rate}':              ['rate==1'],
    'checks{scenario:steady_below_limit}':       ['rate==1'],
    'checks{scenario:route_isolation}':          ['rate==1'],
    'http_req_failed{scenario:steady_below_limit}': ['rate==0'],
  },
};

const URL     = `${GW_BASE}${API_PATH}`;
const HLTH    = `${GW_BASE}/healthz`;
const T       = makeTokens();
const AUTH    = bearerHeader(T.valid);
const BURST_IP  = __ENV.K6_RATE_BURST_IP  || '198.51.100.201';
const REFILL_IP = __ENV.K6_RATE_REFILL_IP || '198.51.100.202';
const STEADY_IP = __ENV.K6_RATE_STEADY_IP || '198.51.100.203';
const ROUTE_IP  = __ENV.K6_RATE_ROUTE_IP  || '198.51.100.204';

function clientHeaders(ip) {
  return Object.assign({ 'X-Forwarded-For': ip }, AUTH);
}

export function setup() {
  const rate = readApiRateLimit();
  if (Math.ceil(rate.capacity * 1.6) > MAX_BURST) {
    throw new Error(`rate_limit.capacity=${rate.capacity} exceeds MAX_BURST=${MAX_BURST}`);
  }
  return rate;
}

// --- A：突发容量上限 ---
// 并发发 CAPACITY*1.5 个请求，瞬时到达才能压过 refill 并验证容量上限。
// 期望：通过数 ≈ CAPACITY（令牌桶满），其余被限流
export function testBurstCapacity(rate) {
  const capacity = rate.capacity;
  const total = Math.ceil(capacity * 1.5);
  let passed = 0, throttled = 0;

  // 先让桶填满（等2s）
  sleep(2);

  const requests = [];
  for (let i = 0; i < total; i++) {
    requests.push(['GET', URL, null, {
      headers: clientHeaders(BURST_IP),
      responseCallback: http.expectedStatuses(200, 429),
    }]);
  }
  let unexpected = 0;
  for (const r of http.batch(requests)) {
    if (r.status === 429) { throttled++; rateLimited.add(1); }
    else if (r.status === 200) { passed++; }
    else { unexpected++; }
  }

  // 突发允许少量时序抖动，但必须接近满桶容量并出现明确限流。
  check({ passed, throttled, total, unexpected }, {
    'burst: passed ≥ 95% capacity':  s => s.passed  >= capacity * 0.95,
    'burst: throttled > 0':          s => s.throttled > 0,
    'burst: throttled ≥ 20% total':  s => s.throttled >= total * 0.2,
    'burst: all responses are 200 or 429': s => s.unexpected === 0,
  });
  console.log(`burst_cap: passed=${passed} throttled=${throttled} total=${total}`);
}

// --- B：令牌桶补充速率 ---
// 先耗尽桶，等 T 秒，验证补充了 T*refill_rps 个 token
export function testRefillRate(rate) {
  const capacity = rate.capacity;
  const refillPerSec = rate.refillPerSec;
  // 耗尽桶
  for (let i = 0; i < capacity + 20; i++) {
    http.get(URL, { headers: clientHeaders(REFILL_IP), responseCallback: http.expectedStatuses(200, 429) });
  }

  // 等1个 refill 周期
  const waitSec = 1.0;
  sleep(waitSec);

  // 此时桶里约有 waitSec*refill_per_sec 个 token
  const expected = Math.floor(waitSec * refillPerSec);
  let passed = 0;
  for (let i = 0; i < expected + 10; i++) {
    const r = http.get(URL, { headers: clientHeaders(REFILL_IP), responseCallback: http.expectedStatuses(200, 429) });
    if (r.status === 200) passed++;
    else break;
  }

  check({ passed, expected }, {
    'refill: recovered tokens ≥ 90% expected': s =>
      s.passed >= Math.floor(s.expected * 0.9),
  });
  console.log(`refill_rate: expected~=${expected} actual_passed=${passed}`);
}

// --- C：稳态低速，不触发限流 ---
export function testSteady(rate) {
  sleep(1 / Math.max(1, Math.floor(rate.refillPerSec * 0.7)));
  const r = http.get(URL, { headers: clientHeaders(STEADY_IP), responseCallback: http.expectedStatuses(200) });
  if (r.status === 429) rateLimited.add(1);
  check(r, { 'steady: no throttle': res => res.status !== 429 });
}

// --- D：路由隔离，/api 限流不影响 /healthz ---
export function testRouteIsolation() {
  // 打满 api 路由
  http.get(URL, { headers: clientHeaders(ROUTE_IP), responseCallback: http.expectedStatuses(200, 429) });
  // healthz 在任何限流状态下都应200（不受 /api route 的限流影响）
  const h = http.get(HLTH, { responseCallback: http.expectedStatuses(200) });
  check(h, { 'route isolation: /healthz always 200': res => res.status === 200 });
}

export default function () {}
