import http from 'k6/http';
import ws from 'k6/ws';
import encoding from 'k6/encoding';
import crypto from 'k6/crypto';
import exec from 'k6/execution';
import { check, sleep } from 'k6';
import { Counter, Rate } from 'k6/metrics';

const BASE = __ENV.BENCH_GATEWAY_URL;
const WS_BASE = __ENV.BENCH_WS_URL;
const DURATION_S = Number(__ENV.BENCH_DURATION_S || 300);
const CONFIGURED_START_MS = Number(__ENV.BENCH_START_EPOCH_MS || 0);
const START_MS = CONFIGURED_START_MS > 0 ? CONFIGURED_START_MS : Date.now();
const SECRET = __ENV.BENCH_JWT_SECRET || 'bench-secret';
const ISSUER = __ENV.BENCH_JWT_ISSUER || 'bench-issuer';
const WARMUP = __ENV.BENCH_MODE === 'warmup';
const SEMANTIC_MIN = Number(__ENV.BENCH_SEMANTIC_MIN || 0.995);
const FAULT_AVAILABLE_MIN = Number(__ENV.BENCH_FAULT_AVAILABLE_MIN || 0.95);
const RECOVERY_AVAILABLE_MIN = Number(__ENV.BENCH_RECOVERY_AVAILABLE_MIN || 0.95);
const RELOAD_START_S = Number(__ENV.BENCH_RELOAD_START_S || DURATION_S + 1);
const CONTROL_GRACE_S = Number(__ENV.BENCH_CONTROL_GRACE_S || 5);
const BAN_TIMELINE = JSON.parse(__ENV.BENCH_BAN_TIMELINE || '[{"at_s":0,"banned":true}]');
const STAGE_RATES = JSON.parse(__ENV.BENCH_STAGE_RATES || '[400,800,1200,1600,2000]');
const STAGE_SECONDS = Number(__ENV.BENCH_STAGE_SECONDS || 60);
const TRAFFIC_WEIGHTS = JSON.parse(__ENV.BENCH_TRAFFIC_WEIGHTS ||
  '{"normal":70,"auth":10,"waf":5,"banned":5,"rate":5,"routing":5}');

const semanticOk = new Rate('bench_semantic_ok');
const normalContractOk = new Rate('bench_normal_contract_ok');
const authContractOk = new Rate('bench_auth_contract_ok');
const wafContractOk = new Rate('bench_waf_contract_ok');
const banContractOk = new Rate('bench_ban_contract_ok');
const rateContractOk = new Rate('bench_rate_contract_ok');
const routingContractOk = new Rate('bench_routing_contract_ok');
const websocketContractOk = new Rate('bench_websocket_contract_ok');
const upstreamContractOk = new Rate('bench_upstream_contract_ok');
const headerContractOk = new Rate('bench_header_contract_ok');
const requestIdContractOk = new Rate('bench_request_id_contract_ok');
const faultAvailable = new Rate('bench_fault_available');
const recoveryAvailable = new Rate('bench_recovery_available');
const unexpected = new Counter('bench_unexpected');
const rateLimited = new Counter('bench_rate_limited');
const wafBlocked = new Counter('bench_waf_blocked');
const banBlocked = new Counter('bench_ban_blocked');
const banAllowed = new Counter('bench_ban_allowed');
const wsEchoed = new Counter('bench_ws_echoed');

function jwt(claims, secret = SECRET) {
  const now = Math.floor(Date.now() / 1000);
  const header = encoding.b64encode(JSON.stringify({ alg: 'HS256', typ: 'JWT' }), 'rawurl');
  const body = encoding.b64encode(JSON.stringify(Object.assign({ iss: ISSUER, iat: now }, claims)), 'rawurl');
  const signature = crypto.hmac('sha256', secret, `${header}.${body}`, 'base64rawurl');
  return `${header}.${body}.${signature}`;
}

const now = Math.floor(Date.now() / 1000);
const TOKENS = {
  valid: jwt({ sub: 'bench-user', exp: now + 3600, scope: 'read write' }),
  expired: jwt({ sub: 'bench-user', exp: now - 60, scope: 'read' }),
  badSecret: jwt({ sub: 'bench-user', exp: now + 3600, scope: 'read' }, 'wrong-secret'),
  wrongIssuer: jwt({ sub: 'bench-user', iss: 'wrong-issuer', exp: now + 3600, scope: 'read' }),
  noScope: jwt({ sub: 'bench-user', exp: now + 3600, scope: 'other' }),
};

function trafficRate(total, name) {
  return Math.max(0, Math.round(total * Number(TRAFFIC_WEIGHTS[name] || 0) / 100));
}

function addArrivalStages(scenarios, name, fn) {
  if (WARMUP) {
    const rps = Number(__ENV.BENCH_WARMUP_RPS || STAGE_RATES[0]);
    const rate = trafficRate(rps, name);
    if (rate > 0) {
      scenarios[`${name}_warmup`] = {
        executor: 'constant-arrival-rate', rate, timeUnit: '1s', duration: `${DURATION_S}s`,
        gracefulStop: '2s',
        preAllocatedVUs: Math.max(4, Math.ceil(rate / 8)), maxVUs: Math.max(16, rate), exec: fn,
        tags: { traffic: name, load_stage: 'warmup', fault_phase: 'none' },
      };
    }
    return;
  }
  STAGE_RATES.forEach((total, index) => {
    const rate = trafficRate(total, name);
    if (rate <= 0) return;
    scenarios[`${name}_stage_${index + 1}`] = {
      executor: 'constant-arrival-rate', rate, timeUnit: '1s',
      startTime: `${index * STAGE_SECONDS}s`, duration: `${STAGE_SECONDS}s`,
      gracefulStop: '2s',
      preAllocatedVUs: Math.max(8, Math.ceil(rate / 8)), maxVUs: Math.max(32, rate), exec: fn,
      tags: { traffic: name, load_stage: `s${index + 1}`, fault_phase: 'timeline' },
    };
  });
}

const scenarios = {};
for (const [name, fn] of [
  ['normal', 'normal'], ['auth', 'auth'], ['waf', 'waf'], ['banned', 'banned'],
  ['rate', 'rateProbe'], ['routing', 'routing'],
]) addArrivalStages(scenarios, name, fn);

const wsClients = Number(__ENV.BENCH_WS_CLIENTS || 0);
if (wsClients > 0 && !WARMUP) {
  scenarios.websocket = {
    executor: 'constant-vus', vus: wsClients, duration: `${DURATION_S}s`, exec: 'websocket',
    gracefulStop: '2s',
    tags: { traffic: 'websocket', load_stage: 'all', fault_phase: 'none' },
  };
}

export const options = {
  scenarios,
  thresholds: WARMUP ? {} : {
    bench_semantic_ok: [`rate>${SEMANTIC_MIN}`],
    bench_auth_contract_ok: ['rate>0.999'],
    bench_waf_contract_ok: ['rate>0.999'],
    bench_ban_contract_ok: ['rate>0.99'],
    bench_rate_contract_ok: ['rate>0.999'],
    bench_normal_contract_ok: [`rate>${SEMANTIC_MIN}`],
    bench_routing_contract_ok: [`rate>${SEMANTIC_MIN}`],
    bench_websocket_contract_ok: [`rate>${SEMANTIC_MIN}`],
    bench_upstream_contract_ok: [`rate>${SEMANTIC_MIN}`],
    bench_header_contract_ok: [`rate>${SEMANTIC_MIN}`],
    bench_request_id_contract_ok: [`rate>${SEMANTIC_MIN}`],
    bench_fault_available: [`rate>${FAULT_AVAILABLE_MIN}`],
    bench_recovery_available: [`rate>${RECOVERY_AVAILABLE_MIN}`],
    bench_waf_blocked: ['count>0'],
    bench_rate_limited: ['count>0'],
    bench_ban_blocked: ['count>0'],
    'http_req_duration{traffic:normal}': ['p(99)<60000'],
    'http_req_duration{traffic:normal,load_stage:s1}': ['p(99)<60000'],
    'http_req_duration{traffic:normal,load_stage:s2}': ['p(99)<60000'],
    'http_req_duration{traffic:normal,load_stage:s3}': ['p(99)<60000'],
    'http_req_duration{traffic:normal,load_stage:s4}': ['p(99)<60000'],
    'http_req_duration{traffic:normal,load_stage:s5}': ['p(99)<60000'],
  },
  discardResponseBodies: false,
  noConnectionReuse: false,
  userAgent: 'bronx-comparison-suite/2',
  summaryTrendStats: ['avg', 'min', 'med', 'max', 'p(90)', 'p(95)', 'p(99)'],
};

export function setup() {
  const waitSeconds = (START_MS - Date.now()) / 1000;
  if (!WARMUP && waitSeconds > 0) sleep(waitSeconds);
}

function xff(prefix, sequence, width = 200) {
  return `${prefix}.${(Number(sequence) % width) + 1}`;
}

function headers(ip, token = TOKENS.valid) {
  const output = {
    'X-Forwarded-For': ip,
    'X-Request-ID': `bench-${exec.scenario.name}-${exec.vu.idInTest}-${exec.scenario.iterationInTest}`,
    'X-Internal-Debug': 'must-be-removed',
  };
  if (token) output.Authorization = `Bearer ${token}`;
  return output;
}

function responseHeader(response, name) {
  const wanted = name.toLowerCase();
  for (const [key, value] of Object.entries(response.headers || {})) {
    if (key.toLowerCase() === wanted) return value;
  }
  return '';
}

function acceptedVersion(value) {
  const elapsed = elapsedSeconds();
  if (elapsed < RELOAD_START_S) return value === 'v1';
  if (elapsed < RELOAD_START_S + CONTROL_GRACE_S) return value === 'v1' || value === 'v2';
  return value === 'v2';
}

function forwardedContract(response, expectedPath, requestHeaders) {
  let payload = null;
  try {
    payload = response.json();
  } catch (_error) {
    payload = null;
  }
  const upstreamOk = payload !== null
    && /^bench-[a-e]$/.test(String(payload.upstream || ''))
    && payload.status === 200
    && payload.method === 'GET'
    && payload.path === expectedPath;
  const forwardedHeaders = payload && payload.headers ? payload.headers : {};
  const headersOk = acceptedVersion(String(forwardedHeaders['x-bench-version'] || ''))
    && String(forwardedHeaders['x-internal-debug'] || '') === '';
  const requestIdOk = responseHeader(response, 'X-Request-ID') === requestHeaders['X-Request-ID']
    && String(forwardedHeaders['x-request-id'] || '') === requestHeaders['X-Request-ID'];
  upstreamContractOk.add(upstreamOk);
  headerContractOk.add(headersOk);
  requestIdContractOk.add(requestIdOk);
  return upstreamOk && headersOk && requestIdOk;
}

function elapsedSeconds() {
  return (Date.now() - START_MS) / 1000;
}

function windows() {
  return JSON.parse(__ENV.BENCH_FAULT_WINDOWS || '[]');
}

function faultActive() {
  const elapsed = elapsedSeconds();
  return windows().some(window => elapsed >= window.start_s && elapsed < window.end_s);
}

function recoveryActive() {
  const elapsed = elapsedSeconds();
  return windows().some(window => elapsed >= window.end_s && elapsed < window.end_s + 5);
}

function record(metric, ok, status) {
  metric.add(ok);
  semanticOk.add(ok);
  if (!ok) unexpected.add(1, { status: String(status) });
}

function recordForwardAvailability(status) {
  if (faultActive()) faultAvailable.add(status === 200);
  if (recoveryActive()) recoveryAvailable.add(status === 200);
}

export function normal() {
  const sequence = exec.scenario.iterationInTest;
  const requestHeaders = headers(xff('198.18.10', sequence));
  const response = http.get(`${BASE}/api/items`, {
    headers: requestHeaders,
    responseCallback: http.expectedStatuses(200, 502, 503, 504),
    tags: { path_kind: 'complete' },
  });
  const statusOk = response.status === 200
    || (faultActive() && [0, 502, 503, 504].includes(response.status));
  const ok = statusOk && (response.status !== 200 || forwardedContract(response, '/items', requestHeaders));
  record(normalContractOk, ok, response.status);
  recordForwardAvailability(response.status);
  check(response, { 'normal response follows phase contract': () => ok });
}

export function auth() {
  const sequence = exec.scenario.iterationInTest;
  const variant = sequence % 5;
  const token = [null, TOKENS.badSecret, TOKENS.expired, TOKENS.wrongIssuer, TOKENS.noScope][variant];
  const expected = variant === 4 ? 403 : 401;
  const response = http.get(`${BASE}/api/items`, {
    headers: headers(xff('198.18.20', sequence), token),
    responseCallback: http.expectedStatuses(401, 403),
    tags: { auth_variant: ['missing', 'bad_secret', 'expired', 'issuer', 'scope'][variant] },
  });
  const ok = response.status === expected;
  record(authContractOk, ok, response.status);
  check(response, { 'auth is always rejected before upstream': () => ok });
}

export function waf() {
  const sequence = exec.scenario.iterationInTest;
  const payloads = ["' OR 1=1 -- ", '<script>alert(1)</script>', '../../etc/passwd', 'sqlmap'];
  const response = http.get(`${BASE}/api/items?q=${encodeURIComponent(payloads[sequence % 4])}`, {
    headers: headers(xff('198.18.30', sequence, 220)), responseCallback: http.expectedStatuses(403),
  });
  if (response.status === 403) wafBlocked.add(1);
  const ok = response.status === 403;
  record(wafContractOk, ok, response.status);
  check(response, { 'waf is always rejected before upstream': () => ok });
}

export function banned() {
  const requestHeaders = headers(__ENV.BENCH_BANNED_IP || '198.18.40.40');
  const response = http.get(`${BASE}/probe/ban`, {
    headers: requestHeaders,
    responseCallback: http.expectedStatuses(200, 403),
  });
  const elapsed = elapsedSeconds();
  let expectedBanned = true;
  for (const transition of BAN_TIMELINE) {
    if (elapsed >= Number(transition.at_s)) expectedBanned = Boolean(transition.banned);
  }
  const expected = expectedBanned ? 403 : 200;
  if (response.status === 403) banBlocked.add(1);
  if (response.status === 200) banAllowed.add(1);
  const inSyncGrace = BAN_TIMELINE.some(transition => Number(transition.at_s) > 0
    && Math.abs(elapsed - Number(transition.at_s)) < CONTROL_GRACE_S);
  const statusOk = response.status === expected || (inSyncGrace && [200, 403].includes(response.status));
  const ok = statusOk && (response.status !== 200 || forwardedContract(response, '/', requestHeaders));
  record(banContractOk, ok, response.status);
  check(response, { 'ban route follows known control state': () => ok });
}

export function rateProbe() {
  const requestHeaders = headers(__ENV.BENCH_RATE_IP || '198.18.50.50');
  const response = http.get(`${BASE}/probe/rate`, {
    headers: requestHeaders,
    responseCallback: http.expectedStatuses(200, 429, 502, 503, 504),
  });
  if (response.status === 429) rateLimited.add(1);
  const statusOk = response.status === 200 || response.status === 429
    || (faultActive() && [0, 502, 503, 504].includes(response.status));
  const ok = statusOk && (response.status !== 200 || forwardedContract(response, '/', requestHeaders));
  record(rateContractOk, ok, response.status);
  check(response, { 'rate route is accepted or throttled': () => ok });
}

export function routing() {
  const sequence = exec.scenario.iterationInTest;
  const paths = ['/exact/1', '/exact/2', '/exact/3', '/exact/4'];
  for (let i = 1; i <= 10; i++) paths.push(`/route/${i}/item`);
  const selectedPath = paths[sequence % paths.length];
  const expectedPath = selectedPath.startsWith('/exact/') ? '/' : '/item';
  const requestHeaders = headers(xff('198.18.60', sequence));
  const response = http.get(`${BASE}${selectedPath}`, {
    headers: requestHeaders,
    responseCallback: http.expectedStatuses(200, 502, 503, 504),
  });
  const statusOk = response.status === 200
    || (faultActive() && [0, 502, 503, 504].includes(response.status));
  const ok = statusOk && (response.status !== 200
    || forwardedContract(response, expectedPath, requestHeaders));
  record(routingContractOk, ok, response.status);
  recordForwardAvailability(response.status);
  check(response, { 'routing response follows phase contract': () => ok });
}

export function websocket() {
  const ip = xff('198.18.70', exec.vu.idInTest, 200);
  let echoed = false;
  const response = ws.connect(`${WS_BASE}/ws/echo`, { headers: headers(ip) }, socket => {
    socket.on('open', () => {
      socket.send('bench-ws');
      socket.setInterval(() => socket.send('bench-ws'), 1000);
    });
    socket.on('message', message => {
      if (message === 'bench-ws') { echoed = true; wsEchoed.add(1); }
    });
    socket.setTimeout(() => socket.close(), 4500);
  });
  const ok = response && response.status === 101 && echoed;
  websocketContractOk.add(ok);
  semanticOk.add(ok);
  check({ ok }, { 'websocket authenticated echo succeeds': value => value.ok });
  sleep(0.5);
}

export default function () {}
