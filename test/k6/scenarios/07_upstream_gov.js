// 上游治理测试：LB 分布 + 熔断三态
//
// 前提：启动多个 mock_rich.py 实例
//   python3 test/api_gw/mock_rich.py --port 8080 &
//   python3 test/api_gw/mock_rich.py --port 8081 &
//   python3 test/api_gw/mock_rich.py --port 8082 &
// 并在 gateway.yml upstreams.endpoints 配置三个节点
import http              from 'k6/http';
import { check, sleep }  from 'k6';
import { GW_BASE, ADMIN_BASE, API_PATH,
         CB_WINDOW_MS, CB_OPEN_TIMEOUT_MS, CB_MIN_REQUESTS,
         MOCK_HOST, MOCK_PORTS } from '../lib/config.js';
import { makeTokens, bearerHeader }          from '../lib/jwt.js';
import { mockHealthyAll, mockFullErrorAll, mockSlowAll,
         mockStatsAll, mockResetStatsAll }   from '../lib/mock.js';
import { cbOpen }                            from '../lib/metrics.js';

export const options = {
  scenarios: {
    upstream_gov: { executor: 'per-vu-iterations', vus: 1, iterations: 1, maxDuration: '8m' },
  },
  thresholds: { checks: ['rate==1'], http_req_failed: ['rate==0'] },
};

const URL  = `${GW_BASE}${API_PATH}`;
const T    = makeTokens();
const AUTH = bearerHeader(T.valid);

function matchesMockPorts(endpoints) {
  return endpoints.every((endpoint, index) => Number(endpoint.port) === Number(MOCK_PORTS[index]));
}

function controlAll(results, label) {
  check(results, { [`${label}: every mock control accepted`]: rows => rows.every(r => r.status === 200) });
}

function readCircuitState() {
  const r = http.get(`${ADMIN_BASE}/routes`, { responseCallback: http.expectedStatuses(200) });
  if (r.status !== 200) return { ok: false, states: [], minRequests: CB_MIN_REQUESTS };
  try {
    const group = (JSON.parse(r.body).upstreams || []).find(item => item.name === 'api');
    if (!group || !Array.isArray(group.endpoints) || group.endpoints.length !== MOCK_PORTS.length
        || !matchesMockPorts(group.endpoints)) {
      return { ok: false, states: [], minRequests: CB_MIN_REQUESTS };
    }
    const endpoints = group.endpoints;
    return {
      ok: true,
      states: endpoints.map(endpoint => endpoint.circuit),
      minRequests: Math.max(...endpoints.map(endpoint => endpoint.circuit_breaker.min_requests || CB_MIN_REQUESTS)),
      halfOpenSuccesses: Math.max(...endpoints.map(endpoint => endpoint.circuit_breaker.half_open_successes || 1)),
    };
  } catch (_e) {
    return { ok: false, states: [], minRequests: CB_MIN_REQUESTS };
  }
}

function sendN(n, expectedStatuses) {
  const counts = {};
  for (let i = 0; i < n; i++) {
    const s = http.get(URL, {
      headers: AUTH,
      responseCallback: http.expectedStatuses(...expectedStatuses),
    }).status;
    counts[s] = (counts[s] || 0) + 1;
  }
  return counts;
}

export function setup() {
  if (MOCK_PORTS.length < 1) throw new Error('K6_MOCK_PORTS cannot be empty');
  const controls = mockHealthyAll(MOCK_PORTS, MOCK_HOST)
    .concat(mockResetStatsAll(MOCK_PORTS, MOCK_HOST));
  if (controls.some(r => r.status !== 200)) throw new Error('cannot initialize every mock');
  const initial = readCircuitState();
  if (!initial.ok) throw new Error('gateway /routes does not expose every configured mock endpoint');
  return { minRequests: initial.minRequests, halfOpenSuccesses: initial.halfOpenSuccesses };
}

export function teardown() {
  mockHealthyAll(MOCK_PORTS, MOCK_HOST);
}

export default function (data) {
  // --- 阶段 1：基线正常，所有 mock 都要收到请求 ---
  sleep(0.5);

  const baseline = sendN(30, [200]);
  check(baseline, {
    'baseline: every request succeeds': b => b[200] === 30,
  });
  const stats = mockStatsAll(MOCK_PORTS, MOCK_HOST);
  check(stats, { 'baseline: every upstream receives traffic': rows =>
    MOCK_PORTS.every(port => (rows[port] || 0) > 0) });

  // --- 阶段 2：触发熔断 ---
  controlAll(mockFullErrorAll(MOCK_PORTS, MOCK_HOST), 'fault setup');
  sleep(0.5);

  const faultRequests = Math.max(data.minRequests + 5, CB_MIN_REQUESTS + 5) * MOCK_PORTS.length;
  sendN(faultRequests, [502, 503, 504]);
  sleep((CB_WINDOW_MS / 1000) + 1);   // 等窗口结算

  const opened = readCircuitState();
  cbOpen.add(opened.states.filter(state => state === 'open').length);
  check(opened, {
    'circuit: every endpoint opens after full upstream errors': s =>
      s.ok && s.states.length === MOCK_PORTS.length && s.states.every(state => state === 'open'),
  });

  // --- 阶段 3：熔断开时请求快速失败 ---
  const duringOpen = sendN(5, [503]);
  check(duringOpen, {
    'during CB open: every request is rejected with 503': d => d[503] === 5,
  });

  // --- 阶段 4：上游恢复，等半开探针成功，熔断关闭 ---
  controlAll(mockHealthyAll(MOCK_PORTS, MOCK_HOST), 'recovery setup');
  sleep((CB_OPEN_TIMEOUT_MS / 1000) + 3);   // 等 open_timeout 过，进入 HALF_OPEN

  const recoveryRequests = Math.max(data.halfOpenSuccesses * MOCK_PORTS.length + 5, 10);
  const recovery = sendN(recoveryRequests, [200]);
  check(recovery, {
    'post-recovery: every request succeeds': r => r[200] === recoveryRequests,
  });
  const recovered = readCircuitState();
  check(recovered, {
    'circuit: every endpoint closes after successful half-open probes': s =>
      s.ok && s.states.every(state => state === 'closed'),
  });

  // --- 阶段 5：慢上游触发慢熔断 ---
  controlAll(mockSlowAll(MOCK_PORTS, MOCK_HOST, 2000, 1.0), 'slow fault setup');
  sleep(0.5);
  sendN(faultRequests, [200, 502, 503, 504]);
  sleep((CB_WINDOW_MS / 1000) + 1);

  const slowOpen = readCircuitState();
  cbOpen.add(slowOpen.states.filter(state => state === 'open').length);
  check(slowOpen, {
    'circuit: every endpoint opens after slow upstream responses': s =>
      s.ok && s.states.every(state => state === 'open'),
  });

  // 清场：后续 LB 场景必须从全部健康、熔断关闭开始，不能继承本场景的故障状态。
  controlAll(mockHealthyAll(MOCK_PORTS, MOCK_HOST), 'slow recovery setup');
  sleep((CB_OPEN_TIMEOUT_MS / 1000) + 3);
  const slowRecovery = sendN(recoveryRequests, [200]);
  check(slowRecovery, {
    'slow recovery: every request succeeds': r => Object.values(r).reduce((sum, count) => sum + count, 0) === r[200],
  });
  const finalState = readCircuitState();
  check(finalState, {
    'slow recovery: every endpoint closes before next scenario': s =>
      s.ok && s.states.every(state => state === 'closed'),
  });
}
