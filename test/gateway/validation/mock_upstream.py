#!/usr/bin/env python3
# mock_upstream.py — 功能完备的 mock 上游，供网关验证用。
# 用法: python3 mock_upstream.py <port> <endpoint_id>
#
# 普通路径：回显 endpoint id + method/host/path/headers/body（JSON）
# 控制接口（通过 header 或 path 触发）：
#   X-Mock-Delay: <ms>        本次响应延迟
#   X-Mock-Status: <code>     本次返回指定状态码
#   X-Mock-Drop: 1            收到请求后直接断开连接（不响应）
#   GET /__ctl/stats          返回本实例统计 {accept_count, active_conns, request_count}
#   GET /__ctl/reset          重置统计与"故障模式"
#   POST /__ctl/fail?n=K      接下来 K 个请求返回 503（模拟故障，熔断测试用）
#   POST /__ctl/recover       立即恢复正常
import sys, json, time, threading
from http.server import BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
import http.server

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8081
EID  = sys.argv[2] if len(sys.argv) > 2 else f"ep-{PORT}"

# 全局统计（原子性靠 GIL + lock）
_lock = threading.Lock()
_accept_count = 0      # TCP accept 次数（新连接）
_active_conns = 0      # 当前活跃连接
_request_count = 0     # HTTP 请求数
_fail_remaining = 0    # 剩余强制失败次数

class Server(ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    def get_request(self):
        global _accept_count, _active_conns
        conn, addr = super().get_request()
        with _lock:
            _accept_count += 1
            _active_conns += 1
        return conn, addr
    def shutdown_request(self, request):
        global _active_conns
        with _lock:
            _active_conns -= 1
        super().shutdown_request(request)

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"   # 支持 keep-alive
    def log_message(self, *a): pass

    def _ctl(self):
        global _fail_remaining, _accept_count, _active_conns, _request_count
        p = self.path.split("?")[0]
        if p == "/__ctl/stats":
            with _lock:
                body = json.dumps({"eid": EID, "accept_count": _accept_count,
                                   "active_conns": _active_conns, "request_count": _request_count}).encode()
            self._send(200, body, "application/json"); return True
        if p == "/__ctl/reset":
            with _lock:
                _accept_count = 0; _request_count = 0; _fail_remaining = 0
            self._send(200, b'{"ok":true}', "application/json"); return True
        if p == "/__ctl/fail":
            q = self.path.split("?",1)[1] if "?" in self.path else ""
            n = 1000000
            for kv in q.split("&"):
                if kv.startswith("n="):
                    try: n = int(kv[2:])
                    except: pass
            with _lock: _fail_remaining = n
            self._send(200, b'{"ok":true}', "application/json"); return True
        if p == "/__ctl/recover":
            with _lock: _fail_remaining = 0
            self._send(200, b'{"ok":true}', "application/json"); return True
        return False

    def _send(self, code, body, ct="text/plain"):
        self.send_response(code)
        self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except Exception: pass

    def _handle(self):
        global _request_count, _fail_remaining
        with _lock: _request_count += 1
        if self._ctl(): return

        # 故障模式
        with _lock:
            if _fail_remaining > 0:
                _fail_remaining -= 1
                self._send(503, b"upstream failure (injected)\n"); return

        # 主动断开
        if self.headers.get("X-Mock-Drop") == "1":
            try: self.connection.close()
            except Exception: pass
            return
        # 延迟
        d = self.headers.get("X-Mock-Delay")
        if d:
            try: time.sleep(int(d)/1000.0)
            except Exception: pass
        # 指定状态码
        status = 200
        s = self.headers.get("X-Mock-Status")
        if s:
            try: status = int(s)
            except Exception: pass
        # 读 body
        clen = int(self.headers.get("Content-Length", 0) or 0)
        body_in = self.rfile.read(clen) if clen > 0 else b""
        # 回显（含 echo-id 便于串包检测）
        eid_hdr = self.headers.get("X-Echo-Id", "")
        resp = {
            "endpoint": EID,
            "method": self.command,
            "host": self.headers.get("Host",""),
            "path": self.path,
            "echo_id": eid_hdr,
            "headers": {k: v for k,v in self.headers.items()},
            "body_len": len(body_in),
        }
        out = json.dumps(resp).encode()
        self._send(status, out, "application/json")

    def do_GET(self):    self._handle()
    def do_POST(self):   self._handle()
    def do_PUT(self):    self._handle()
    def do_DELETE(self): self._handle()
    def do_PATCH(self):  self._handle()

if __name__ == "__main__":
    srv = Server(("127.0.0.1", PORT), H)
    print(f"mock upstream {EID} on :{PORT}", flush=True)
    srv.serve_forever()
