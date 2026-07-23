import http from 'k6/http';
import { ADMIN_BASE, API_PATH } from './config.js';

function getJson(path) {
  const r = http.get(`${ADMIN_BASE}${path}`, {
    responseCallback: http.expectedStatuses(200),
  });
  if (r.status !== 200) throw new Error(`admin ${path} returned ${r.status}`);
  try {
    return JSON.parse(r.body);
  } catch (_e) {
    throw new Error(`admin ${path} did not return JSON`);
  }
}

export function readGatewayStats() {
  return getJson('/stats');
}

export function readGatewayRoutes() {
  return getJson('/routes');
}

function routeMatches(route, path) {
  if (route.match_type === 'exact') return route.path === path;
  if (route.match_type === 'prefix') {
    const prefix = route.path.endsWith('/') ? route.path : `${route.path}/`;
    return path === route.path || path.startsWith(prefix);
  }
  return false;
}

// 从被测网关的管理面读取限流参数，避免脚本常量与实际 YAML 漂移。
export function readApiRateLimit() {
  const routes = readGatewayRoutes().routes;
  if (!Array.isArray(routes)) throw new Error('admin /routes has no routes array');

  const candidates = routes.filter(route =>
    routeMatches(route, API_PATH) && route.rate_limit && route.rate_limit.enabled);
  if (candidates.length === 0) {
    throw new Error(`no enabled rate_limit route matches ${API_PATH}`);
  }
  const route = candidates.sort((a, b) => b.path.length - a.path.length)[0];
  const capacity = Number(route.rate_limit.capacity);
  const refillPerSec = Number(route.rate_limit.refill_per_sec);
  if (!Number.isFinite(capacity) || capacity <= 0
      || !Number.isFinite(refillPerSec) || refillPerSec <= 0
      || route.rate_limit.key !== 'ip') {
    throw new Error(`invalid IP rate_limit for route ${route.name || route.path}`);
  }
  return { capacity, refillPerSec, route: route.name || route.path };
}
