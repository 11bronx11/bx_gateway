from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import sys, time
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_GET(self):
        if self.path.startswith("/slow"):
            time.sleep(0.05)
        # 回显 X-Echo-Id（串包检测用）；否则默认响应
        eid = self.headers.get("X-Echo-Id", "")
        b = (f"echo-id={eid}\n").encode() if eid else b"hello from upstream\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n) if n else b""
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a): pass
port = int(sys.argv[1]) if len(sys.argv) > 1 else 8081
ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
