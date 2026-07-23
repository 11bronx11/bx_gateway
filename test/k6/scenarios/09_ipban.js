// IP 封禁测试
//
// 两条路径：
//   A. banctl 预封（需 run_ipban.sh 包装）：K6_PREBANNED=1 时验证403
//   B. WAF 命中自动举报封禁（需 WAF、Hub 和可信代理链同时可用）
//
// Hub HTTP 健康/指标也在这里验证
import http              from 'k6/http';
import { check, sleep }  from 'k6';
import { GW_BASE, ADMIN_BASE, API_PATH,
         HUB_BASE }      from '../lib/config.js';
import { makeTokens, bearerHeader } from '../lib/jwt.js';
import { readGatewayStats } from '../lib/admin.js';

const PREBANNED = (__ENV.K6_PREBANNED || '0') === '1';
const AUTO_BAN = (__ENV.K6_AUTO_BAN || '0') === '1';
const TEST_IP = __ENV.K6_IPBAN_TEST_IP || '';
const ALLOW_IP = __ENV.K6_IPBAN_ALLOW_IP || '';
const VERIFY_RECOVERY = (__ENV.K6_IPBAN_RECOVERY || '0') === '1';
const WAF_BAN_MS = parseInt(__ENV.K6_WAF_BAN_MS || '600000');

export const options = {
  scenarios: {
    // A: 预封路径（run_ipban.sh 调用时生效）
    prebanned: PREBANNED ? {
      executor: 'per-vu-iterations', vus: 1, iterations: 1,
      maxDuration: '2m', exec: 'testPrebanned',
    } : { executor: 'per-vu-iterations', vus: 0, iterations: 0, exec: 'testPrebanned' },
    // B: WAF 自动封禁路径
    waf_autoban: AUTO_BAN ? {
      executor: 'per-vu-iterations', vus: 1, iterations: 1,
      maxDuration: '3m', exec: 'testWafAutoban',
    } : { executor: 'per-vu-iterations', vus: 0, iterations: 0, exec: 'testWafAutoban' },
    // C: Hub 健康/指标（始终跑）
    hub_health: {
      executor: 'per-vu-iterations', vus: 1, iterations: 1,
      maxDuration: '30s', exec: 'testHubHealth',
    },
    // D: 静态 ip_filter（始终跑，验非封禁 IP 正常通行）
    static_allow: {
      executor: 'constant-arrival-rate', rate: 10, timeUnit: '1s',
      duration: '30s', preAllocatedVUs: 5, exec: 'testStaticAllow',
    },
  },
  thresholds: { checks: ['rate==1'], http_req_failed: ['rate==0'] },
};

const URL  = `${GW_BASE}${API_PATH}`;
const T    = makeTokens();
const AUTH = bearerHeader(T.valid);

export function setup() {
  const stats = readGatewayStats();
  if (typeof stats.waf_denied !== 'number' || typeof stats.ipban_denied !== 'number') {
    throw new Error('admin /stats does not expose WAF and IP-ban counters');
  }
  return { wafDenied: stats.waf_denied, ipbanDenied: stats.ipban_denied };
}

function clientHeaders(headers = {}) {
  return TEST_IP ? Object.assign({ 'X-Forwarded-For': TEST_IP }, headers) : headers;
}

function allowHeaders(headers = {}) {
  if (ALLOW_IP) return Object.assign({ 'X-Forwarded-For': ALLOW_IP }, headers);
  return VERIFY_RECOVERY ? clientHeaders(headers) : headers;
}

const WAF_PAYLOADS = ["' OR 1=1--", "UNION SELECT null,null", "<script>xss</script>"];

// A: banctl 预封——本机 IP 被封，所有请求应返回 403，且不需要鉴权头
export function testPrebanned() {
  // 无鉴权头，BanMiddleware 在 RouteAuth 前拦截
  const r1 = http.get(URL, {
    headers: clientHeaders(),
    responseCallback: http.expectedStatuses(403),
  });
  check(r1, {
    'prebanned: no-auth still 403': res => res.status === 403,
    'prebanned: connection header closed': res =>
      (res.headers['Connection'] || '').toLowerCase() === 'close',
  });

  // 有效鉴权头也一样
  const r2 = http.get(URL, {
    headers: clientHeaders(AUTH),
    responseCallback: http.expectedStatuses(403),
  });
  check(r2, { 'prebanned: valid jwt still 403': res => res.status === 403 });

  // /healthz 也应被封禁（BanMiddleware 在 HealthCheck 前）
  const r3 = http.get(`${GW_BASE}/healthz`, {
    headers: clientHeaders(),
    responseCallback: http.expectedStatuses(403),
  });
  check(r3, { 'prebanned: /healthz also 403': res => res.status === 403 });
}

// B: WAF 自动举报触发封禁（需 WAF、Hub 和可信代理链可用）
export function testWafAutoban(data) {
  if (!TEST_IP) throw new Error('K6_IPBAN_TEST_IP is required for WAF autoban');

  // 先证这个 XFF 尚未被封，否则后续的 403 不能归因于本次 WAF 攻击。
  const before = http.get(URL, {
    headers: clientHeaders(AUTH),
    responseCallback: http.expectedStatuses(200),
  });
  check(before, { 'waf_autoban: test IP is allowed before injection': res => res.status === 200 });

  // 一次命中必须同时留下 WAF 计数和后续封禁两个可观察证据。
  const q = WAF_PAYLOADS[0];
  const attack = http.get(`${URL}?q=${encodeURIComponent(q)}`, {
    headers: clientHeaders(),
    responseCallback: http.expectedStatuses(403),
  });
  check(attack, { 'waf_autoban: injection is blocked by WAF': res => res.status === 403 });
  const afterWaf = readGatewayStats();
  check({ before: data.wafDenied, after: afterWaf.waf_denied }, {
    'waf_autoban: gateway waf counter increased': s => s.after > s.before,
  });

  // 等 WAF 风险上报经 hub 同步回网关，轮询而非固定睡眠。
  let banned = false;
  let finalStatus = 0;
  for (let i = 0; i < 10; i++) {
    sleep(1);
    const r = http.get(URL, {
      headers: clientHeaders(AUTH),
      responseCallback: http.expectedStatuses(200, 403),
    });
    finalStatus = r.status;
    if (r.status === 403) {
      banned = true;
      break;
    }
  }
  check({ banned, finalStatus }, {
    'waf_autoban: reported client becomes banned': s => s.banned && s.finalStatus === 403,
  });
  const afterBan = readGatewayStats();
  check({ before: data.ipbanDenied, after: afterBan.ipban_denied }, {
    'waf_autoban: gateway IP-ban counter increased': s => s.after > s.before,
  });

  // 测试环境可把 WAF ban_ms 配为短 TTL，再验证解封。
  if (WAF_BAN_MS <= 30000) {
    sleep(WAF_BAN_MS / 1000 + 2);
    const recovered = http.get(URL, {
      headers: clientHeaders(AUTH),
      responseCallback: http.expectedStatuses(200),
    });
    check(recovered, { 'waf_autoban: ban expired → 200': res => res.status === 200 });
  }
}

// C: Hub 健康和 Prometheus 指标
export function testHubHealth() {
  const healthz = http.get(`${HUB_BASE}/healthz`, { responseCallback: http.expectedStatuses(200) });
  check(healthz, { 'hub /healthz 200': res => res.status === 200 });

  const metrics = http.get(`${HUB_BASE}/metrics`, { responseCallback: http.expectedStatuses(200) });
  check(metrics, {
    'hub /metrics 200': res => res.status === 200,
    'hub metrics is text': res => (res.headers['Content-Type'] || '').includes('text'),
    'hub metrics has ipban_rules': res => res.body.includes('ipban_rules'),
  });
}

// D: 未封禁 IP 正常通行（基线验证）
export function testStaticAllow() {
  const r = http.get(URL, {
    headers: allowHeaders(AUTH),
    responseCallback: http.expectedStatuses(200),
  });
  check(r, { 'non-banned ip: reaches upstream with 200': res => res.status === 200 });
}

export default function () {}
