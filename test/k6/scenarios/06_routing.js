import http      from 'k6/http';
import { check } from 'k6';
import { GW_BASE } from '../lib/config.js';
import { makeTokens, bearerHeader } from '../lib/jwt.js';

export const options = {
  scenarios: {
    routing: { executor: 'constant-arrival-rate', rate: 10, timeUnit: '1s', duration: '2m', preAllocatedVUs: 5 },
  },
  thresholds: { checks: ['rate==1'], http_req_failed: ['rate==0'] },
};

const T = makeTokens();

function upstreamPath(r, expected) {
  try {
    return r.status === 200 && JSON.parse(r.body).path === expected;
  } catch (_e) { return false; }
}

export default function () {
  const auth = bearerHeader(T.valid);

  // prefix strip: /api/users → 上游必须实际收到 /users
  {
    const r = http.get(`${GW_BASE}/api/users`, {
      headers: auth,
      responseCallback: http.expectedStatuses(200),
    });
    check(r, { '/api/users strips prefix before upstream': res => upstreamPath(res, '/users') });
  }

  // 路径不匹配 → 404
  {
    const r = http.get(`${GW_BASE}/no-such-route-xyz`, { responseCallback: http.expectedStatuses(404) });
    check(r, { 'unknown path → 404': res => res.status === 404 });
  }

  // method 过滤：gateway.yml route methods=[GET,POST,PUT,PATCH]，DELETE 不在其中。
  // 网关无 405 语义——method 不匹配即路由未命中 → 404（router.cpp::ruleMatches）。
  {
    const r = http.del(`${GW_BASE}/api/users/1`, null, {
      headers: auth,
      responseCallback: http.expectedStatuses(404),
    });
    check(r, { 'DELETE (not in methods) → 404': res => res.status === 404 });
  }

  // 允许的 method（PUT 在 methods 内）应正常路由，不被 method 过滤挡掉
  {
    const r = http.put(`${GW_BASE}/api/users/1`, null, {
      headers: auth,
      responseCallback: http.expectedStatuses(200),
    });
    check(r, { 'PUT (in methods) reaches stripped upstream path': res => upstreamPath(res, '/users/1') });
  }

  // 带 query string 的路由不影响 prefix 匹配
  {
    const r = http.get(`${GW_BASE}/api/users?page=1&size=20`, {
      headers: auth,
      responseCallback: http.expectedStatuses(200),
    });
    check(r, { 'query string reaches stripped upstream path': res =>
      upstreamPath(res, '/users?page=1&size=20') });
  }

  // 健康检查直接由 HealthCheck 中间件响应，不需鉴权
  {
    const r = http.get(`${GW_BASE}/healthz`, { responseCallback: http.expectedStatuses(200) });
    check(r, { '/healthz → 200': res => res.status === 200 });
  }

  // admin 接口在业务口不可达（9090 才有）
  {
    const r = http.get(`${GW_BASE}/stats`, { responseCallback: http.expectedStatuses(404) });
    check(r, { '/stats on business port → 404': res => res.status === 404 });
  }
}
