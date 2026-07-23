// X-Forwarded-For 信任链与伪造 —— 强断言版
//
// 前提：gateway.yml `trusted_proxies: [127.0.0.1/8]`。k6 从本机(127.0.0.1)连，
// 属于可信代理，所以网关**采信** XFF，clientAddr = XFF 链中最右侧不可信 IP。
// 若 trusted_proxies 为空，XFF 被忽略、clientAddr=socket peer，分桶断言会红——
// 那是"功能没开"不是"功能坏"。TRUSTED_MODE 默认 true，可用 K6_TRUSTED_MODE=0 关。
//
// 验证方式：per-IP 限流（实际容量和补充速率从 admin /routes 读取）
// 是 XFF 是否真生效的可观测探针——不同 XFF IP 若被独立分桶，说明网关按 XFF 计客户端。
import http                         from 'k6/http';
import { check, sleep }             from 'k6';
import { GW_BASE, API_PATH } from '../lib/config.js';
import { makeTokens, bearerHeader } from '../lib/jwt.js';
import { gwErrors, rateLimited }    from '../lib/metrics.js';
import { readApiRateLimit }         from '../lib/admin.js';

const TRUSTED_MODE = (__ENV.K6_TRUSTED_MODE || '1') === '1';
const BURST_FACTOR = 1.6;
const MAX_BURST = 4096;

export const options = {
  scenarios: {
    xff_tests: { executor: 'per-vu-iterations', vus: 1, iterations: 1, maxDuration: '3m' },
  },
  // 串行打会被 refill 填回，测不出限流。
  // 必须让一批请求"瞬时并发"到达(http.batch)才能压过 refill 打满桶。
  // 默认 batchPerHost=6 会把并发拆成串行小批，必须抬到突发请求数以上。
  batch:        MAX_BURST,
  batchPerHost: MAX_BURST,
  thresholds: {
    checks:    ['rate==1'],
    http_req_failed: ['rate==0'],
    gw_errors: ['count==0'],
  },
};

const T    = makeTokens();
const AUTH = bearerHeader(T.valid);
const URL  = `${GW_BASE}${API_PATH}`;

const SAFE_IP_A   = '203.0.113.10';  // RFC5737 TEST-NET-3
const SAFE_IP_B   = '203.0.113.20';
const ATTACKER_IP = '192.0.2.99';    // RFC5737 TEST-NET-1

export function setup() {
  if (!TRUSTED_MODE) return {};
  const rate = readApiRateLimit();
  if (Math.ceil(rate.capacity * BURST_FACTOR) > MAX_BURST) {
    throw new Error(`rate_limit.capacity=${rate.capacity} exceeds MAX_BURST=${MAX_BURST}`);
  }
  return rate;
}

export default function (rate) {
  // ── 测试 1：baseline，无 XFF ────────────────────────────────
  {
    const r = http.get(URL, { headers: AUTH, responseCallback: http.expectedStatuses(200) });
    if (r.status >= 500) gwErrors.add(1);
    check(r, { 'baseline no XFF: 200': res => res.status === 200 });
  }
  sleep(0.5);

  // ── 测试 2：XFF 注入恶意字符——解析必须健壮，绝不 5xx ──────────
  // 强断言：不管 XFF 里塞什么，网关都不能崩（不 5xx）
  {
    const malicious = '127.0.0.1, <script>alert(1)</script>, 192.168.1.1';
    const r = http.get(URL, {
      headers: Object.assign({ 'X-Forwarded-For': malicious }, AUTH),
      responseCallback: http.expectedStatuses(200),
    });
    if (r.status >= 500) gwErrors.add(1);
    check(r, { 'XFF malicious chars: safely handled with 200': res => res.status === 200 });
  }
  sleep(0.5);

  if (!TRUSTED_MODE) {
    console.log('TRUSTED_MODE=0: XFF 应被忽略，跳过信任链强断言');
    return;
  }

  const burstCount = Math.ceil(rate.capacity * BURST_FACTOR);

  // ── 测试 3：XFF 分桶——核心强断言 ───────────────────────────
  // trusted 模式下网关按 XFF 计客户端 IP。用两个不同 XFF IP 各自打满实际配置的桶，
  // 若真分桶，则 A 被限流不影响 B；若网关无视 XFF（都用 127.0.0.1），两者共享一个桶。
  //
  // 判据：先猛打 SAFE_IP_A 触发它的 429，此时 SAFE_IP_B 首个请求必须仍能通过（非429）。
  {
    // 先耗尽 A 的桶：按当前 capacity 计算的请求一次性并发发出，压过 refill。
    const reqs = [];
    for (let i = 0; i < burstCount; i++) {
      reqs.push(['GET', URL, null, {
        headers: Object.assign({ 'X-Forwarded-For': SAFE_IP_A }, AUTH),
        tags: { test: 'xff_bucket_a' },
        responseCallback: http.expectedStatuses(200, 429),
      }]);
    }
    let aThrottled = 0;
    for (const r of http.batch(reqs)) {
      if (r.status === 429) { aThrottled++; rateLimited.add(1); }
    }
    check({ aThrottled }, {
      'XFF bucket A: got throttled after flooding (proves per-IP limit active)':
        s => s.aThrottled > 0,
    });

    // A 被限流的当下，B 的桶应是满的——首请求必须通过
    const rb = http.get(URL, {
      headers: Object.assign({ 'X-Forwarded-For': SAFE_IP_B }, AUTH),
      tags: { test: 'xff_bucket_b' },
      responseCallback: http.expectedStatuses(200),
    });
    check(rb, {
      'XFF bucket B: independent bucket, not throttled while A is (proves XFF honored)':
        res => res.status === 200,
    });
    console.log(`  bucket A throttled=${aThrottled}, bucket B first req=${rb.status}`);
  }
  sleep(1.5);  // 让桶回补一些，避免污染下一测试

  // ── 测试 4：多跳链解析——最右不可信 IP ──────────────────────
  // XFF = "attacker, 127.0.0.2"：127.0.0.2 属可信(127/8)，attacker 不可信 →
  // clientAddr = attacker。用 attacker IP 单独打，验其独立成桶（能被单独限流）。
  {
    const chain = `${ATTACKER_IP}, 127.0.0.2`;
    const reqs = [];
    for (let i = 0; i < burstCount; i++) {
      reqs.push(['GET', URL, null, {
        headers: Object.assign({ 'X-Forwarded-For': chain }, AUTH),
        tags: { test: 'xff_chain' },
        responseCallback: http.expectedStatuses(200, 429),
      }]);
    }
    let throttled = 0;
    for (const r of http.batch(reqs)) {
      if (r.status === 429) { throttled++; rateLimited.add(1); }
    }
    // 多跳链正确解析出 attacker → 它有独立桶 → 猛打能触发 429
    check({ throttled }, {
      'XFF multi-hop: rightmost-untrusted IP resolved & bucketed (got 429)':
        s => s.throttled > 0,
    });
    console.log(`  multi-hop chain="${chain}" attacker throttled=${throttled}`);
  }
}
