import { Counter, Gauge } from 'k6/metrics';

// 网关业务指标
export const gwErrors      = new Counter('gw_errors');        // 非预期错误（5xx）
export const rateLimited   = new Counter('gw_rate_limited');  // 429
export const authRejected  = new Counter('gw_auth_rejected'); // 401/403
export const wafBlocked    = new Counter('gw_waf_blocked');   // WAF 403
export const cbOpen        = new Gauge('gw_circuit_open');    // 熔断计数快照
export const wsFrames      = new Counter('gw_ws_frames');     // WS 帧收发数
export const wsReplies     = new Counter('gw_ws_replies');    // 收到的 WS 回显帧
export const upstreamHits  = new Counter('gw_upstream_hits'); // 实际到达上游数（从 mock stats 读）
export const slowHeld      = new Counter('gw_slow_held');     // 受控慢上游实际占用等待的请求数
