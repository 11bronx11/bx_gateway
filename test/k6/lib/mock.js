// mock_rich.py 控制助手，通过 /_soak/control 动态改上游行为
import http from 'k6/http';
import { MOCK_BASE } from './config.js';

const CTRL  = `${MOCK_BASE}/_soak/control`;
const STATS = `${MOCK_BASE}/_soak/stats`;

const JSON_HDR = { headers: { 'Content-Type': 'application/json' } };

// 一次性设置多个参数
export function mockConfigure(params) {
  return http.post(CTRL, JSON.stringify(params), JSON_HDR);
}

// 常用快捷
export function mockHealthy() {
  return mockConfigure({
    healthy: true,
    error_probability: 0,
    drop_probability: 0,
    slow_probability: 0,
    clear_pending_errors: true,
  });
}

export function mockFullError() {
  return mockConfigure({ error_probability: 1.0 });
}

export function mockDropAll() {
  return mockConfigure({ drop_probability: 1.0 });
}

export function mockSlow(delayMs = 2000, prob = 1.0) {
  return mockConfigure({ slow_probability: prob, slow_delay_ms: delayMs });
}

export function mockPartialError(prob = 0.5) {
  return mockConfigure({ error_probability: prob });
}

// 读当前 mock 统计
export function mockStats() {
  const r = http.get(STATS);
  if (r.status !== 200) return null;
  try { return JSON.parse(r.body); } catch (_e) { return null; }
}

// 重置统计计数
export function mockResetStats() {
  return mockConfigure({ reset_stats: true });
}

// --- 多实例操作（LB 测试用）---

// 对所有端口广播同一配置
export function mockConfigureAll(ports, host, params) {
  const results = [];
  for (const port of ports) {
    results.push(http.post(
      `http://${host}:${port}/_soak/control`,
      JSON.stringify(params),
      { headers: { 'Content-Type': 'application/json' } }
    ));
  }
  return results;
}

export function mockHealthyAll(ports, host) {
  return mockConfigureAll(ports, host, {
    healthy: true,
    error_probability: 0,
    drop_probability: 0,
    slow_probability: 0,
    clear_pending_errors: true,
  });
}

export function mockFullErrorAll(ports, host) {
  return mockConfigureAll(ports, host, { error_probability: 1.0 });
}

export function mockSlowAll(ports, host, delayMs = 2000, prob = 1.0) {
  return mockConfigureAll(ports, host, {
    slow_probability: prob,
    slow_delay_ms: delayMs,
  });
}

// 读取所有实例的请求数，返回 {port: count} map
export function mockStatsAll(ports, host) {
  const result = {};
  for (const port of ports) {
    const r = http.get(`http://${host}:${port}/_soak/stats`);
    if (r.status === 200) {
      try {
        const stats = JSON.parse(r.body);
        const count = stats.requests !== undefined ? stats.requests : stats.requests_total;
        result[port] = typeof count === 'number' && count >= 0 ? count : -1;
      }
      catch (_e) { result[port] = -1; }
    } else {
      result[port] = -1;
    }
  }
  return result;
}

// 重置所有实例统计
export function mockResetStatsAll(ports, host) {
  return mockConfigureAll(ports, host, { reset_stats: true });
}
