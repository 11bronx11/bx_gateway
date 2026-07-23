// 所有可调参数从环境变量读，保持默认值与 gateway.yml 一致

export const GW_HOST    = __ENV.K6_GW_HOST    || '127.0.0.1';
export const GW_PORT    = __ENV.K6_GW_PORT    || '8090';
export const ADMIN_HOST = __ENV.K6_ADMIN_HOST || '127.0.0.1';
export const ADMIN_PORT = __ENV.K6_ADMIN_PORT || '9090';
export const MOCK_HOST  = __ENV.K6_MOCK_HOST  || '127.0.0.1';
export const MOCK_PORT  = __ENV.K6_MOCK_PORT  || '8080';

// LB 多实例：K6_MOCK_PORTS="8080,8081,8082"，默认只有一个节点
// 格式：逗号分隔端口号，顺序与 gateway.yml upstreams.endpoints 一致
export const MOCK_PORTS = (__ENV.K6_MOCK_PORTS || MOCK_PORT)
  .split(',').map(p => p.trim()).filter(Boolean);

export const GW_BASE    = `http://${GW_HOST}:${GW_PORT}`;
export const ADMIN_BASE = `http://${ADMIN_HOST}:${ADMIN_PORT}`;
export const MOCK_BASE  = `http://${MOCK_HOST}:${MOCK_PORT}`;
export const WS_BASE    = `ws://${GW_HOST}:${GW_PORT}`;

// auth
export const JWT_SECRET = __ENV.K6_JWT_SECRET || 'change-me';
export const JWT_ISSUER = __ENV.K6_JWT_ISSUER || 'bronx';
export const API_KEY    = __ENV.K6_API_KEY    || '';

// 业务路由（与 gateway.yml routes 对应）
export const API_PATH   = __ENV.K6_API_PATH   || '/api/users';
export const WS_PATH    = __ENV.K6_WS_PATH    || '/api/ws';

// 熔断参数（与 gateway.yml circuit_breaker 对应，用于测试等待窗口）
export const CB_WINDOW_MS       = parseInt(__ENV.K6_CB_WINDOW_MS || '10000');
export const CB_OPEN_TIMEOUT_MS = parseInt(__ENV.K6_CB_OPEN_TIMEOUT_MS || '5000');
export const CB_MIN_REQUESTS    = parseInt(__ENV.K6_CB_MIN_REQUESTS || '10');

// 健康长稳的正常流量 SLO。压测报告必须记录实际传入值。
export const NORMAL_RPS    = parseInt(__ENV.K6_NORMAL_RPS || '40');
export const NORMAL_P99_MS = parseInt(__ENV.K6_NORMAL_P99_MS || '250');
export const RATE_PROBE_INTERVAL = __ENV.K6_RATE_PROBE_INTERVAL || '5m';

// 组合场景以可信代理模式给破坏性探针分配独立逻辑客户端，避免污染正常流量桶。
export const RATE_PROBE_IP = __ENV.K6_RATE_PROBE_IP || '198.51.100.240';
export const WAF_PROBE_IP = __ENV.K6_WAF_PROBE_IP || '198.51.100.230';
export const WAF_SAFE_IP  = __ENV.K6_WAF_SAFE_IP  || '198.51.100.231';

// 每次 WAF 攻击分配一个独立可信 XFF，避免先前自动封禁把后续 403 伪装成 WAF 命中。
export function uniqueWafProbeIp(sequence) {
  const parts = WAF_PROBE_IP.split('.');
  if (parts.length !== 4) return WAF_PROBE_IP;
  const n = Math.max(0, Number(sequence) || 0);
  return `${parts[0]}.${parts[1]}.${parts[2]}.${(n % 100) + 1}`;
}

// 限流突发也会触发风险上报。每轮使用另一个测试 IP，避免已封桶污染后续恢复断言。
export function uniqueRateProbeIp(sequence) {
  const parts = RATE_PROBE_IP.split('.');
  if (parts.length !== 4) return RATE_PROBE_IP;
  const n = Math.max(0, Number(sequence) || 0);
  return `${parts[0]}.${parts[1]}.${parts[2]}.${(n % 100) + 101}`;
}

// hub HTTP 可观测端口（可选，hub.yml 配置 http_addr 时才有）
export const HUB_HOST = __ENV.K6_HUB_HOST || '127.0.0.1';
export const HUB_PORT = __ENV.K6_HUB_PORT || '9091';
export const HUB_BASE = `http://${HUB_HOST}:${HUB_PORT}`;
