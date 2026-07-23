// LB 分布验证
//
// 前提：
//   1. 启动 N 个 mock_rich.py，端口与 K6_MOCK_PORTS 对应
//      python3 test/api_gw/mock_rich.py --port 8080 &
//      python3 test/api_gw/mock_rich.py --port 8081 &
//      python3 test/api_gw/mock_rich.py --port 8082 &
//   2. gateway.yml upstreams.endpoints 配置相同 N 个节点
//   3. K6_MOCK_PORTS=8080,8081,8082 k6 run 08_lb_distribution.js
//
// 至少需要两个端口，单节点无法证明负载分布
import http              from 'k6/http';
import { check, sleep }  from 'k6';
import { GW_BASE, ADMIN_BASE, API_PATH,
         MOCK_HOST, MOCK_PORTS }             from '../lib/config.js';
import { makeTokens, bearerHeader }          from '../lib/jwt.js';
import { mockResetStatsAll, mockStatsAll,
         mockHealthyAll }                    from '../lib/mock.js';
import { upstreamHits }                      from '../lib/metrics.js';

const SEND_COUNT = parseInt(__ENV.K6_LB_SEND || '300');  // 总请求数，默认300
const TOLERANCE = parseFloat(__ENV.K6_LB_TOLERANCE || '0.20');

export const options = {
  scenarios: {
    lb_dist: {
      executor: 'per-vu-iterations', vus: 1, iterations: 1, maxDuration: '3m',
    },
  },
  thresholds: {
    checks: ['rate==1'],
    http_req_failed: ['rate==0'],
    gw_upstream_hits: ['count>0'],
  },
};

const URL  = `${GW_BASE}${API_PATH}`;
const T    = makeTokens();
const AUTH = bearerHeader(T.valid);

function matchesMockPorts(endpoints) {
  return endpoints.every((endpoint, index) => Number(endpoint.port) === Number(MOCK_PORTS[index]));
}

export function setup() {
  if (MOCK_PORTS.length < 2) {
    throw new Error('LB distribution requires at least two K6_MOCK_PORTS');
  }
  const controls = mockHealthyAll(MOCK_PORTS, MOCK_HOST)
    .concat(mockResetStatsAll(MOCK_PORTS, MOCK_HOST));
  if (controls.some(r => r.status !== 200)) {
    throw new Error('cannot reset every mock before LB distribution test');
  }

  const routes = http.get(`${ADMIN_BASE}/routes`, { responseCallback: http.expectedStatuses(200) });
  if (routes.status !== 200) throw new Error(`cannot read gateway routes, status=${routes.status}`);
  let group;
  try {
    group = (JSON.parse(routes.body).upstreams || []).find(item => item.name === 'api');
  } catch (_e) {
    throw new Error('gateway /routes is not valid JSON');
  }
  if (!group || !Array.isArray(group.endpoints) || group.endpoints.length !== MOCK_PORTS.length
      || !matchesMockPorts(group.endpoints)) {
    throw new Error('gateway api upstream endpoints do not match K6_MOCK_PORTS');
  }

  sleep(0.5);
  return {
    ports: MOCK_PORTS,
    host: MOCK_HOST,
    n: SEND_COUNT,
    lb: group.lb,
    weights: group.endpoints.map(endpoint => endpoint.weight || 1),
  };
}

export default function (data) {
  for (let i = 0; i < data.n; i++) {
    const r = http.get(URL, {
      headers: AUTH,
      responseCallback: http.expectedStatuses(200),
    });
    if (r.status === 200) upstreamHits.add(1);
    check(r, { 'lb: request reaches upstream with 200': res => res.status === 200 });
  }
}

export function teardown(data) {
  sleep(1);
  const stats = mockStatsAll(data.ports, data.host);
  const readable = Object.values(stats).every(value => value >= 0);
  const total = Object.values(stats).reduce((a, b) => a + (b > 0 ? b : 0), 0);
  const n     = data.ports.length;

  console.log(`lb_distribution: total_hits=${total} across ${n} nodes`);
  data.ports.forEach(p => console.log(`  port=${p} hits=${stats[p]}`));

  check({ readable, total }, {
    'lb: all mock statistics are readable': s => s.readable,
    'lb: upstream received every sent request': s => s.total >= data.n,
  });

  if (data.lb !== 'weighted_least_conn') {
    const weighted = data.lb === 'weighted';
    const totalWeight = data.weights.reduce((sum, weight) => sum + weight, 0);
    const expectedHits = data.ports.map((_port, index) =>
      weighted ? total * data.weights[index] / totalWeight : total / n);
    let balanced = true;

    data.ports.forEach((port, index) => {
      const hits = stats[port] || 0;
      const expected = expectedHits[index];
      const dev = expected ? Math.abs(hits - expected) / expected : 0;
      if (dev > TOLERANCE) {
        console.warn(`lb imbalance: port=${port} hits=${hits} expected~=${expected.toFixed(0)} dev=${(dev * 100).toFixed(1)}%`);
        balanced = false;
      }
    });

    check({ balanced }, {
      [`lb: ${data.lb} distribution within ${TOLERANCE * 100}%`]: s => s.balanced,
    });
  } else {
    console.log('lb_distribution: weighted_least_conn skips fixed-ratio checks under serial load');
  }

  // 没有节点命中数为 0（节点被完全跳过）
  const dead = data.ports.filter(p => (stats[p] || 0) === 0);
  check({ dead }, { 'lb: no dead node': s => s.dead.length === 0 });
}
