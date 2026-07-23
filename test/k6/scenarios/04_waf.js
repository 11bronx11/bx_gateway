// WAF 强断言：注入向量必须被拦(403)，合法请求必须放行(非403)
//
// 前提：gateway.yml ip_policy.waf.enabled=true（否则 WAF 中间件根本不装链，
//       所有注入都会穿透到上游，此测试会大面积红——那是"功能没开"不是"功能坏"）。
//
// WAF 实现边界（waf.cpp::inspect）：只扫 url path + query + UA/Referer/Cookie 三个 header。
//   **不扫 body**。所以 JSON body 注入即使开了 WAF 也不会被拦——不在这里做假断言。
import http      from 'k6/http';
import { check } from 'k6';
import { GW_BASE, API_PATH, WAF_SAFE_IP, uniqueWafProbeIp } from '../lib/config.js';
import { makeTokens, bearerHeader } from '../lib/jwt.js';
import { wafBlocked } from '../lib/metrics.js';
import { readGatewayStats } from '../lib/admin.js';

export const options = {
  scenarios: {
    waf: { executor: 'per-vu-iterations', vus: 1, iterations: 1,
           maxDuration: '1m' },
  },
  // 强断言全过才算真绿：注入必拦 + 合法必放
  thresholds: {
    checks: ['rate==1'],
    http_req_failed: ['rate==0'],
    gw_waf_blocked: ['count>0'],
  },
};

const BASE = `${GW_BASE}${API_PATH}`;
const T    = makeTokens();

function attackHeaders(headers = {}, slot = 0) {
  const sequence = ((__VU - 1) * 1000000) + (__ITER * 32) + slot;
  return Object.assign({ 'X-Forwarded-For': uniqueWafProbeIp(sequence) }, headers);
}

function safeHeaders(headers = {}) {
  return Object.assign({ 'X-Forwarded-For': WAF_SAFE_IP }, headers);
}

// query 注入向量——每条都匹配 waf.cpp 的某条规则，开 WAF 后必须 403
const QUERY_PAYLOADS = [
  { tag: 'sqli_or',       q: "1 OR 1=1" },              // sqli: or\s+1\s*=\s*1
  { tag: 'sqli_union',    q: 'UNION SELECT null,null-- ' },// sqli: union select / '-- '
  { tag: 'sqli_sleep',    q: "1;SLEEP(5)" },            // sqli: sleep\s*\(
  { tag: 'xss_script',    q: '<script>alert(1)</script>' },// xss: <script
  { tag: 'xss_onerror',   q: '"><img src=x onerror=alert(1)>' },// xss: onerror= / <img..src
  { tag: 'xss_svg',       q: '<svg onload=alert(1)>' },  // xss: onload=
  { tag: 'path_trav',     q: '../../etc/passwd' },       // travers: ../ + /etc/passwd
  { tag: 'path_trav_enc', q: '%2e%2e%2fetc%2fpasswd' },  // travers: unescape 后 ../etc/passwd
  { tag: 'scanner_sqlmap',q: 'sqlmap' },                 // scanner: sqlmap
  { tag: 'scanner_nikto', q: 'nikto' },                  // scanner: nikto
];

// header 注入——UA/Referer/Cookie 是 WAF 监视的头，塞攻击串应被拦
const HEADER_PAYLOADS = [
  { tag: 'hdr_ua_sqli',     hdr: { 'User-Agent': "sqlmap/1.0" } },       // scanner
  { tag: 'hdr_referer_xss', hdr: { 'Referer': '<script>alert(1)</script>' } },// xss
  { tag: 'hdr_cookie_sqli', hdr: { 'Cookie': "sid=1 OR 1=1" } },         // sqli: or\s+1\s*=\s*1
];

// 合法请求——绝不能被 WAF 误拦（无假阳性）
const SAFE_QUERIES = [
  { tag: 'safe_normal',  q: 'hello world' },
  { tag: 'safe_unicode', q: '你好世界' },
  { tag: 'safe_json_q',  q: '{"key":"value"}' },
  { tag: 'safe_email',   q: 'user@example.com' },
];

export function setup() {
  const stats = readGatewayStats();
  if (typeof stats.waf_denied !== 'number') throw new Error('admin /stats has no waf_denied counter');
  return { wafDenied: stats.waf_denied };
}

export default function () {
  // query 注入：必须被拦（403）
  for (let i = 0; i < QUERY_PAYLOADS.length; i++) {
    const p = QUERY_PAYLOADS[i];
    const r = http.get(`${BASE}?q=${encodeURIComponent(p.q)}`, {
      headers: attackHeaders({}, i),
      responseCallback: http.expectedStatuses(403),
    });
    const blocked = r.status === 403;
    if (blocked) wafBlocked.add(1);
    check(r, {
      [`waf ${p.tag}: injection blocked (403)`]: () => blocked,
      [`waf ${p.tag}: block body not empty`]:   res => !blocked || (res.body || '').length > 0,
    });
  }

  // header 注入：UA/Referer/Cookie 里的攻击串必须被拦
  for (let i = 0; i < HEADER_PAYLOADS.length; i++) {
    const p = HEADER_PAYLOADS[i];
    const r = http.get(BASE, {
      headers: attackHeaders(p.hdr, QUERY_PAYLOADS.length + i),
      responseCallback: http.expectedStatuses(403),
    });
    const blocked = r.status === 403;
    if (blocked) wafBlocked.add(1);
    check(r, { [`waf ${p.tag}: header injection blocked (403)`]: () => blocked });
  }

  // 合法请求：绝不能误拦（带合法 JWT，走到上游或鉴权，就是不能 403 被 WAF 拦）
  for (const p of SAFE_QUERIES) {
    const r = http.get(`${BASE}?q=${encodeURIComponent(p.q)}`,
      {
        headers: safeHeaders(bearerHeader(T.valid)),
        responseCallback: http.expectedStatuses(200),
      });
    check(r, { [`waf ${p.tag}: safe request reaches upstream (200)`]: res => res.status === 200 });
  }
}

export function teardown(data) {
  const stats = readGatewayStats();
  check({ before: data.wafDenied, after: stats.waf_denied }, {
    'waf: gateway waf_denied counter increased for every attack vector': s =>
      s.after - s.before >= QUERY_PAYLOADS.length + HEADER_PAYLOADS.length,
  });
}
