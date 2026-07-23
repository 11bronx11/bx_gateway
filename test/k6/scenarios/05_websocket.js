import ws        from 'k6/ws';
import http      from 'k6/http';
import { check, sleep } from 'k6';
import { WS_BASE, GW_BASE } from '../lib/config.js';
import { makeTokens } from '../lib/jwt.js';
import { wsFrames, wsReplies } from '../lib/metrics.js';

export const options = {
  scenarios: {
    // 成功路径：握手 + 帧收发
    ws_clients: {
      executor: 'constant-vus', vus: 5, duration: '3m',
      exec: 'wsNormal',
    },
    // 鉴权拒绝路径：无效/缺失 token 不应握手成功
    ws_auth_reject: {
      executor: 'per-vu-iterations', vus: 1, iterations: 1,
      maxDuration: '1m', exec: 'wsAuthReject',
      startTime: '10s',
    },
  },
  thresholds: {
    'gw_ws_frames': ['count>0'],
    'checks{exec:wsNormal}':      ['rate==1'],
    'checks{exec:wsAuthReject}':  ['rate==1'],
    gw_ws_replies:                ['count>0'],
  },
};

const T = makeTokens();

// ── 成功路径 ──────────────────────────────────────────────────
export function wsNormal() {
  const url = `${WS_BASE}/api/ws`;
  const params = { headers: { Authorization: `Bearer ${T.valid}` }, tags: { exec: 'wsNormal' } };
  let received = 0;

  const res = ws.connect(url, params, function (socket) {
    let received = 0;

    socket.on('open', () => {
      // 发几轮 ping/echo 帧
      for (let i = 0; i < 5; i++) {
        socket.send(JSON.stringify({ type: 'ping', seq: i }));
        wsFrames.add(1);
      }
    });

    socket.on('message', (data) => {
      received++;
      wsFrames.add(1);
      wsReplies.add(1);
      try {
        const msg = JSON.parse(data);
        check(msg, { 'ws message has type field': m => m.type !== undefined });
      } catch (_e) { /* 文本帧 */ }
      if (received >= 5) socket.close();
    });

    socket.on('error', (e) => {
      check(null, { [`ws no error: ${e}`]: () => false });
    });

    // 30s 超时兜底
    socket.setTimeout(() => socket.close(), 30000);
  });

  check(res, { 'ws handshake 101': r => r.status === 101 });
  check({ received }, { 'ws receives all sent echo frames': s => s.received === 5 });
  sleep(1);
}

// ── 鉴权拒绝路径 ─────────────────────────────────────────────
// WS 升级前 gateway 应鉴权；无效 token 应在 HTTP 握手阶段被拒（非 101）
export function wsAuthReject() {
  const url = `${WS_BASE}/api/ws`;

  // 1. 无鉴权头 → 应被拒（401/403，不握手）
  {
    const r = ws.connect(url, {}, (socket) => { socket.close(); });
    check(r, {
      'ws no token: not 101': res => res.status !== 101,
      'ws no token: 401 or 403': res => res.status === 401 || res.status === 403,
    });
    console.log(`  [ws no token] → ${r.status}`);
  }

  sleep(0.5);

  // 2. 过期 JWT → 应被拒
  {
    const r = ws.connect(url,
      { headers: { Authorization: `Bearer ${T.expired}` } },
      (socket) => { socket.close(); },
    );
    check(r, {
      'ws expired token: not 101': res => res.status !== 101,
      'ws expired token: 401': res => res.status === 401,
    });
    console.log(`  [ws expired] → ${r.status}`);
  }

  sleep(0.5);

  // 3. 错误 secret 签名 → 应被拒
  {
    const r = ws.connect(url,
      { headers: { Authorization: `Bearer ${T.badSecret}` } },
      (socket) => { socket.close(); },
    );
    check(r, {
      'ws bad secret: not 101': res => res.status !== 101,
      'ws bad secret: 401': res => res.status === 401,
    });
    console.log(`  [ws badSecret] → ${r.status}`);
  }

  sleep(0.5);

  // 4. alg:none 攻击 → 应被拒
  {
    const r = ws.connect(url,
      { headers: { Authorization: `Bearer ${T.algNone}` } },
      (socket) => { socket.close(); },
    );
    check(r, {
      'ws alg:none: not 101': res => res.status !== 101,
      'ws alg:none: 401': res => res.status === 401,
    });
    console.log(`  [ws algNone] → ${r.status}`);
  }

  sleep(0.5);

  // 5. 正常 token 对照组 → 握手成功
  {
    const r = ws.connect(url,
      { headers: { Authorization: `Bearer ${T.valid}` } },
      (socket) => { sleep(0.1); socket.close(); },
    );
    check(r, { 'ws valid token: 101': res => res.status === 101 });
    console.log(`  [ws valid] → ${r.status}`);
  }
}

export default function () {}
