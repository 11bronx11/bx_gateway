import http from 'k6/http';
import encoding from 'k6/encoding';
import crypto from 'k6/crypto';
import exec from 'k6/execution';
import { check, sleep } from 'k6';
import { Counter, Rate } from 'k6/metrics';

const BASE = __ENV.BENCH_GATEWAY_URL;
const SECRET = __ENV.BENCH_JWT_SECRET;
const ISSUER = __ENV.BENCH_JWT_ISSUER;
const MODE = __ENV.BENCH_MODE || 'formal';
const WARMUP = MODE === 'warmup';
const STAGE_RATES = JSON.parse(__ENV.BENCH_STAGE_RATES || '[500,1000,2000,4000,8000,16000,32000]');
const TARGET_RPS = Number(__ENV.BENCH_TARGET_RPS || STAGE_RATES[0]);
const STAGE_INDEX = Number(__ENV.BENCH_STAGE_INDEX || 1);
const STAGE_SECONDS = Number(__ENV.BENCH_STAGE_SECONDS || 60);
const WARMUP_SECONDS = Number(__ENV.BENCH_WARMUP_S || 30);
const START_MS = Number(__ENV.BENCH_START_EPOCH_MS || 0);

const contractOk = new Rate('bench_contract_ok');
const unexpected = new Counter('bench_unexpected');
const rateLimited = new Counter('bench_rate_limited');

function jwt() {
  const now = Math.floor(Date.now() / 1000);
  const header = encoding.b64encode(JSON.stringify({ alg: 'HS256', typ: 'JWT' }), 'rawurl');
  const claims = encoding.b64encode(JSON.stringify({
    iss: ISSUER, sub: 'capacity-user', scope: 'read write', iat: now, exp: now + 3600,
  }), 'rawurl');
  const signature = crypto.hmac('sha256', SECRET, `${header}.${claims}`, 'base64rawurl');
  return `${header}.${claims}.${signature}`;
}

const TOKEN = jwt();
const scenarios = {};
if (WARMUP) {
  scenarios.warmup = {
    executor: 'constant-arrival-rate',
    rate: STAGE_RATES[0],
    timeUnit: '1s',
    duration: `${WARMUP_SECONDS}s`,
    gracefulStop: '2s',
    preAllocatedVUs: Math.max(16, Math.ceil(STAGE_RATES[0] / 20)),
    maxVUs: Math.max(64, STAGE_RATES[0]),
    exec: 'healthy',
    tags: { load_stage: 'warmup' },
  };
} else {
  scenarios[`stage_${STAGE_INDEX}`] = {
    executor: 'constant-arrival-rate',
    rate: TARGET_RPS,
    timeUnit: '1s',
    duration: `${STAGE_SECONDS}s`,
    gracefulStop: '2s',
    preAllocatedVUs: Math.max(32, Math.ceil(TARGET_RPS / 50)),
    maxVUs: Math.max(128, Math.ceil(TARGET_RPS / 5)),
    exec: 'healthy',
    tags: { load_stage: `s${STAGE_INDEX}` },
  };
}

const thresholds = {};
if (!WARMUP) {
  const stage = `s${STAGE_INDEX}`;
  thresholds[`bench_contract_ok{load_stage:${stage}}`] = ['rate>=0'];
  thresholds[`bench_unexpected{load_stage:${stage}}`] = ['count>=0'];
  thresholds[`bench_rate_limited{load_stage:${stage}}`] = ['count>=0'];
  thresholds[`http_reqs{load_stage:${stage}}`] = ['count>=0'];
  thresholds[`http_req_duration{load_stage:${stage}}`] = ['p(99)<60000'];
  thresholds[`dropped_iterations{load_stage:${stage}}`] = ['count>=0'];
}

export const options = {
  scenarios,
  thresholds,
  discardResponseBodies: false,
  noConnectionReuse: false,
  userAgent: 'bronx-capacity-2c2g/1',
  summaryTrendStats: ['avg', 'min', 'med', 'max', 'p(90)', 'p(95)', 'p(99)'],
};

export function setup() {
  if (!WARMUP && START_MS > Date.now()) sleep((START_MS - Date.now()) / 1000);
}

function responseHeader(response, name) {
  const wanted = name.toLowerCase();
  for (const [key, value] of Object.entries(response.headers || {})) {
    if (key.toLowerCase() === wanted) return value;
  }
  return '';
}

export function healthy() {
  const requestId = `capacity-${exec.scenario.name}-${exec.vu.idInTest}-${exec.scenario.iterationInTest}`;
  const response = http.get(`${BASE}/api/items`, {
    headers: {
      Authorization: `Bearer ${TOKEN}`,
      'X-Forwarded-For': '198.18.90.90',
      'X-Request-ID': requestId,
      'X-Internal-Debug': 'must-be-removed',
    },
    responseCallback: http.expectedStatuses(200, 429, 502, 503, 504),
  });
  let payload = null;
  try { payload = response.json(); } catch (_error) { payload = null; }
  const forwarded = payload && payload.headers ? payload.headers : {};
  const ok = response.status === 200
    && payload !== null
    && ['bench-a', 'bench-b'].includes(String(payload.upstream || ''))
    && payload.status === 200
    && payload.method === 'GET'
    && payload.path === '/items'
    && String(forwarded['x-bench-version'] || '') === 'v1'
    && String(forwarded['x-internal-debug'] || '') === ''
    && String(forwarded['x-request-id'] || '') === requestId
    && responseHeader(response, 'X-Request-ID') === requestId;
  contractOk.add(ok);
  if (response.status === 429) rateLimited.add(1);
  if (!ok) unexpected.add(1, { status: String(response.status) });
  check(response, { 'healthy request satisfies full contract': () => ok });
}
