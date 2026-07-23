// 慢速攻击测试
// 目标：验证网关在大量长连接下仍能正常服务正常客户端
// WS 静默长连接霸占连接槽位，同时观察正常请求和攻击后的健康状态。
import http                         from 'k6/http';
import ws                           from 'k6/ws';
import { check, sleep }             from 'k6';
import { GW_BASE, API_PATH, WS_PATH } from '../lib/config.js';
import { makeTokens, bearerHeader }  from '../lib/jwt.js';
import { gwErrors }                  from '../lib/metrics.js';

export const options = {
  scenarios: {
    // 攻击方：开 WS 连接后静默不发帧（霸占连接槽位）
    slow_ws_hold: {
      executor:    'constant-vus',
      vus:         __ENV.K6_SLOW_WS_VUS  ? parseInt(__ENV.K6_SLOW_WS_VUS)  : 20,
      duration:    '90s',
      exec:        'holdWs',
      startTime:   '0s',
    },
    // 正常客户端：同期持续发请求，验可用性不受影响
    normal_during_hold: {
      executor:    'constant-arrival-rate',
      rate:        20,   // 20 req/s
      timeUnit:    '1s',
      duration:    '90s',
      preAllocatedVUs: 5,
      exec:        'normalRequest',
      startTime:   '5s', // 攻击建立后再开始计量
    },
    // 阶段结束后少量验证，确认网关仍健康
    health_check: {
      executor:    'per-vu-iterations',
      vus:         1,
      iterations:  5,
      exec:        'healthCheck',
      startTime:   '95s',
    },
  },
  thresholds: {
    'checks{scenario:slow_ws_hold}':             ['rate==1'],
    'checks{scenario:normal_during_hold}':       ['rate==1'],
    'http_req_duration{scenario:normal_during_hold}': ['p(95)<3000'],
    'checks{scenario:health_check}':             ['rate==1'],
    http_req_failed:                              ['rate==0'],
    gw_errors:                                   ['count==0'],
  },
};

const T    = makeTokens();
const AUTH = bearerHeader(T.valid);
const GW_WS = GW_BASE.replace(/^http/, 'ws');

// 攻击者：建立 WS 连接，握手成功后什么都不发，尽量持续占用
export function holdWs() {
  const url = `${GW_WS}${WS_PATH}`;
  const done = ws.connect(url, { headers: AUTH }, (socket) => {
    // 不发任何帧，只是持有连接
    socket.setTimeout(() => {
      socket.close();
    }, 60000); // 最多持 60s（比场景时长短，防止超时报错）
  });
  // 空闲关闭可以发生在 60s 后，但攻击连接必须先完成 101 握手。
  check(done, {
    'ws hold: handshake 101': r => r.status === 101,
  });
  sleep(1);
}

// 正常客户端：发普通 GET，验证返回正常
export function normalRequest() {
  const r = http.get(`${GW_BASE}${API_PATH}`, {
    headers: AUTH,
    timeout: '5s',
    tags: { scenario: 'normal_during_hold' },
    responseCallback: http.expectedStatuses(200),
  });

  if (r.status >= 500) gwErrors.add(1);

  check(r, {
    'normal req: served with 200': res => res.status === 200,
  });
}

// 健康检查：等攻击结束后确认 /healthz 正常
export function healthCheck() {
  const r = http.get(`${GW_BASE}/healthz`, {
    timeout: '3s',
    responseCallback: http.expectedStatuses(200),
  });
  check(r, {
    'post-attack healthz: 200': res => res.status === 200,
  });
  sleep(1);
}

export default function () {}
