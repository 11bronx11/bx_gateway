#!/usr/bin/env python3
# 阶段二压测用的最小上游。多线程 HTTP/1.1 keep-alive,固定小响应,近零自身开销
# ——这样瓶颈和延迟归因才落在网关+toxiproxy 那跳,不是上游自己慢。
# 路由:/users /echo /public 都回 200 小 JSON;/slow 睡 N 毫秒(?ms=)模拟慢上游。
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
import time

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"          # keep-alive 复用连接 逼网关连接池真跑起来
    def log_message(self, *a): pass        # 静音 别拖慢

    def _body(self):
        u = urlparse(self.path)
        if u.path == "/slow":
            q = parse_qs(u.query)
            ms = int(q.get("ms", ["0"])[0]) if q.get("ms") else 0
            if ms > 0: time.sleep(ms / 1000.0)
        return b'{"ok":true,"path":"' + u.path.encode("ascii", "replace") + b'"}'

    def _respond(self):
        body = self._body()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  self._respond()
    def do_POST(self):
        # 把请求 body 读干净 否则 keep-alive 下会串包
        n = int(self.headers.get("Content-Length", 0) or 0)
        if n: self.rfile.read(n)
        self._respond()

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    srv.daemon_threads = True
    print(f"mock upstream on 127.0.0.1:{port}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
