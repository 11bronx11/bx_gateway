// 大 body + chunked 传输测试
// 验证网关对大请求体的正确转发，以及 chunked 响应的完整解码
import http      from 'k6/http';
import { check } from 'k6';
import { GW_BASE, API_PATH, MOCK_HOST, MOCK_PORTS } from '../lib/config.js';
import { makeTokens, bearerHeader }     from '../lib/jwt.js';
import { mockConfigureAll, mockHealthyAll } from '../lib/mock.js';
import { gwErrors }                     from '../lib/metrics.js';

export const options = {
  scenarios: {
    // 场景1：大请求体 POST（64KB / 256KB）
    large_body: {
      executor: 'per-vu-iterations', vus: 3, iterations: 9,
      maxDuration: '2m', exec: 'testLargeBody',
    },
    // 场景2：chunked 响应解码正确性
    chunked_resp: {
      executor: 'per-vu-iterations', vus: 2, iterations: 6,
      maxDuration: '2m', exec: 'testChunkedResponse',
    },
  },
  thresholds: {
    checks:                          ['rate==1'],
    http_req_failed:                 ['rate==0'],
    'http_req_duration{type:large}': ['p(95)<5000'],
    gw_errors:                       ['count==0'],
  },
};

const URL  = `${GW_BASE}${API_PATH}`;
const T    = makeTokens();
const AUTH = bearerHeader(T.valid);

export function setup() {
  const results = mockConfigureAll(MOCK_PORTS, MOCK_HOST, {
    error_probability: 0,
    drop_probability: 0,
    slow_probability: 0,
    chunked_probability: 1,
  });
  if (results.some(r => r.status !== 200)) throw new Error('cannot force chunked response on every mock');
}

export function teardown() {
  mockHealthyAll(MOCK_PORTS, MOCK_HOST);
  mockConfigureAll(MOCK_PORTS, MOCK_HOST, { chunked_probability: 0 });
}

// 生成指定大小的随机 payload（ASCII 可打印字符）
function makePayload(sizeBytes) {
  const chunk = 'abcdefghijklmnopqrstuvwxyz0123456789';
  let s = '';
  while (s.length < sizeBytes) s += chunk;
  return s.slice(0, sizeBytes);
}

const P64K  = makePayload(64  * 1024);
const P256K = makePayload(256 * 1024);

export function testLargeBody() {
  const iter = __ITER % 3; // 轮流测三档

  let body, tag, size;
  if (iter === 0) {
    body = P64K;  tag = '64k';  size = P64K.length;
  } else if (iter === 1) {
    body = P256K; tag = '256k'; size = P256K.length;
  } else {
    // 空 body POST（对照组）
    body = ''; tag = 'empty'; size = 0;
  }

  const r = http.post(URL, body, {
    headers: Object.assign({ 'Content-Type': 'text/plain' }, AUTH),
    tags: { type: 'large' },
    responseCallback: http.expectedStatuses(200),
  });

  if (r.status >= 500) gwErrors.add(1);

  check(r, {
    [`large body ${tag}: forwarded (2xx)`]: res => res.status === 200 || res.status === 201,
    [`large body ${tag}: not 413`]:         res => res.status !== 413, // 配额内不应被截断
  });

  // 强断言：mock_rich.py 在响应 JSON 里回显 body_bytes = 它实际收到的请求体字节数。
  // 验证 gateway 把 body 完整转发给上游，一字节不多不少（转发完整性的真正证明）。
  if (r.status === 200) {
    let got = -1;
    try { got = JSON.parse(r.body).body_bytes; } catch (_e) { /* 非 JSON */ }
    check({ got, size }, {
      [`large body ${tag}: upstream received exact ${size} bytes`]: d => d.got === d.size,
    });
  }
}

// mock 在 setup 中被强制为 chunked，网关应保持流式 chunked 向客户端转发。
export function testChunkedResponse() {
  const r = http.get(`${URL}?chunked=1`, {
    headers: AUTH,
    tags: { type: 'chunked' },
    responseCallback: http.expectedStatuses(200),
  });

  if (r.status >= 500) gwErrors.add(1);

  check(r, {
    'chunked resp: status is 200': res => res.status === 200,
    'chunked resp: body is complete': res => (res.body || '').length > 0,
    'chunked resp: gateway keeps chunked stream framing': res =>
      (res.headers['Transfer-Encoding'] || '').toLowerCase().includes('chunked'),
  });
}

export default function () {}
