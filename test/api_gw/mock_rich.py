#!/usr/bin/env python3
"""Fault-injecting HTTP/1.1 upstream for Bronx gateway soak tests."""

import argparse
import base64
import collections
import hashlib
import json
import random
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class MockState:
    def __init__(self, args):
        self.lock = threading.RLock()
        self.name = args.name
        self.rng = random.Random(args.seed)
        self.config = {
            "healthy": args.healthy,
            "error_probability": args.error_probability,
            "error_burst": args.error_burst,
            "drop_probability": args.drop_probability,
            "chunked_probability": args.chunked_probability,
            "slow_probability": args.slow_probability,
            "slow_delay_ms": args.slow_delay_ms,
            "delay_p50_ms": args.delay_p50_ms,
            "delay_p99_ms": args.delay_p99_ms,
            "max_normal_delay_ms": args.max_normal_delay_ms,
            "body_sizes": args.body_sizes,
            "not_found_path": args.not_found_path,
        }
        self.pending_errors = 0
        self.requests = 0
        self.health_requests = 0
        self.health_failures = 0
        self.drops = 0
        self.slow_requests = 0
        self.responses = collections.Counter()
        self.recent_requests = collections.deque(maxlen=128)
        self.ws_sessions = 0
        self.ws_frames = 0
        self.tcp_accepts = 0
        self.connection_closes = 0
        self.active_connections = 0
        self.max_active_connections = 0
        self.reused_requests = 0
        self.connection_requests = {}

    def configure(self, values):
        probabilities = {
            "error_probability", "drop_probability", "chunked_probability", "slow_probability",
        }
        delays = {"slow_delay_ms", "delay_p50_ms", "delay_p99_ms", "max_normal_delay_ms"}
        allowed = probabilities | delays | {
            "healthy", "error_burst", "clear_pending_errors", "reset_stats", "seed",
        }
        unknown = set(values) - allowed
        if unknown:
            raise ValueError("unknown controls: " + ", ".join(sorted(unknown)))
        with self.lock:
            next_config = dict(self.config)
            if "healthy" in values:
                if not isinstance(values["healthy"], bool):
                    raise ValueError("healthy must be a boolean")
                next_config["healthy"] = values["healthy"]
            for name in probabilities:
                if name in values:
                    value = float(values[name])
                    if not 0.0 <= value <= 1.0:
                        raise ValueError(name + " must be 0..1")
                    next_config[name] = value
            for name in delays:
                if name in values:
                    value = float(values[name])
                    if value < 0.0:
                        raise ValueError(name + " cannot be negative")
                    next_config[name] = value
            if next_config["delay_p99_ms"] < next_config["delay_p50_ms"]:
                raise ValueError("delay_p99_ms must be >= delay_p50_ms")
            if next_config["max_normal_delay_ms"] < next_config["delay_p50_ms"]:
                raise ValueError("max_normal_delay_ms must be >= delay_p50_ms")
            if "error_burst" in values:
                value = int(values["error_burst"])
                if value < 1:
                    raise ValueError("error_burst must be >= 1")
                next_config["error_burst"] = value
            if "clear_pending_errors" in values and not isinstance(values["clear_pending_errors"], bool):
                raise ValueError("clear_pending_errors must be a boolean")
            self.config = next_config
            if "seed" in values:
                self.rng = random.Random(int(values["seed"]))
            if values.get("clear_pending_errors"):
                self.pending_errors = 0
            if values.get("reset_stats"):
                self.pending_errors = 0
                self.requests = 0
                self.health_requests = 0
                self.health_failures = 0
                self.drops = 0
                self.slow_requests = 0
                self.responses.clear()
                self.tcp_accepts = 0
                self.connection_closes = 0
                self.max_active_connections = self.active_connections
                self.reused_requests = 0

    def connection_opened(self, connection_id):
        with self.lock:
            self.tcp_accepts += 1
            self.active_connections += 1
            self.max_active_connections = max(self.max_active_connections, self.active_connections)
            self.connection_requests[connection_id] = 0

    def connection_closed(self, connection_id):
        with self.lock:
            self.connection_closes += 1
            self.active_connections = max(0, self.active_connections - 1)
            self.connection_requests.pop(connection_id, None)

    def record_health_request(self, healthy):
        with self.lock:
            self.health_requests += 1
            if not healthy:
                self.health_failures += 1

    def record_data_request(self, connection_id):
        with self.lock:
            self.requests += 1
            previous = self.connection_requests.get(connection_id, 0)
            if previous > 0:
                self.reused_requests += 1
            self.connection_requests[connection_id] = previous + 1

    def response_plan(self):
        """Reserve a fully deterministic request outcome before any sleep or I/O."""
        with self.lock:
            config = self.config
            if self.rng.random() < config["drop_probability"]:
                self.drops += 1
                return {"drop": True}
            if self.rng.random() < config["slow_probability"]:
                delay_ms = config["slow_delay_ms"]
                self.slow_requests += 1
            else:
                sigma = max(0.0, (config["delay_p99_ms"] - config["delay_p50_ms"]) / 2.326347874)
                delay_ms = min(
                    config["max_normal_delay_ms"],
                    max(0.5, self.rng.gauss(config["delay_p50_ms"], sigma)),
                )
            if self.pending_errors:
                self.pending_errors -= 1
                status = 503
            elif self.rng.random() < config["error_probability"]:
                self.pending_errors = config["error_burst"] - 1
                status = 503
            else:
                status = 200
            return {
                "drop": False,
                "delay_ms": delay_ms,
                "status": status,
                "chunked": self.rng.random() < config["chunked_probability"],
                "body_size": self.rng.choice(config["body_sizes"]),
                "not_found_path": config["not_found_path"],
            }

    def record_response(self, status):
        with self.lock:
            self.responses[str(status)] += 1

    def record_request(self, method, path, headers):
        interesting = (
            "authorization", "x-api-key", "x-user-id", "x-user-scopes", "x-user-roles",
            "x-forwarded-for", "x-real-ip", "x-soak-probe", "upgrade", "connection",
            "x-request-id", "x-bench-version", "x-internal-debug",
        )
        with self.lock:
            self.recent_requests.append({
                "method": method,
                "path": path,
                "headers": {name: headers.get(name, "") for name in interesting},
            })

    def record_ws_session(self):
        with self.lock:
            self.ws_sessions += 1

    def record_ws_frame(self):
        with self.lock:
            self.ws_frames += 1

    def snapshot(self):
        with self.lock:
            return {
                "name": self.name,
                "requests": self.requests,
                "data_requests": self.requests,
                "health_requests": self.health_requests,
                "health_failures": self.health_failures,
                "drops": self.drops,
                "slow_requests": self.slow_requests,
                "responses": dict(self.responses),
                "recent_requests": list(self.recent_requests),
                "ws_sessions": self.ws_sessions,
                "ws_frames": self.ws_frames,
                "tcp_accepts": self.tcp_accepts,
                "connection_closes": self.connection_closes,
                "active_connections": self.active_connections,
                "max_active_connections": self.max_active_connections,
                "reused_requests": self.reused_requests,
                "pending_errors": self.pending_errors,
                "config": dict(self.config),
            }

    def healthy(self):
        with self.lock:
            return self.config["healthy"]


class RichHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    @property
    def state(self):
        return self.server.state

    def setup(self):
        super().setup()
        self._connection_id = id(self.connection)
        self.state.connection_opened(self._connection_id)
        try:
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass

    def finish(self):
        try:
            super().finish()
        finally:
            self.state.connection_closed(self._connection_id)

    def do_GET(self):
        if self.path == "/_soak/stats":
            self._send_json(200, self.state.snapshot())
            return
        if self.path == "/_soak/health":
            healthy = self.state.healthy()
            self.state.record_health_request(healthy)
            self._send_json(200 if healthy else 503, {"name": self.state.name, "healthy": healthy})
            return
        if self.headers.get("Upgrade", "").lower() == "websocket":
            self._websocket()
            return
        self._reply(0)

    def do_POST(self):
        if self.path == "/_soak/control":
            self._control()
            return
        self._reply_with_body()

    def do_PUT(self):
        self._reply_with_body()

    def do_PATCH(self):
        self._reply_with_body()

    def do_DELETE(self):
        self._reply_with_body()

    def _reply_with_body(self):
        content_length = self._content_length()
        if content_length is None:
            return
        self.rfile.read(content_length)
        self._reply(content_length)

    def _content_length(self):
        try:
            content_length = int(self.headers.get("Content-Length", "0") or 0)
        except ValueError:
            self._send_json(400, {"error": "invalid content length"})
            return None
        if content_length < 0 or content_length > 64 * 1024 * 1024:
            self._send_json(413, {"error": "content length out of range"})
            return None
        return content_length

    def _reply(self, request_body_bytes):
        self.state.record_data_request(self._connection_id)
        self.state.record_request(self.command, self.path, {
            name.lower(): value for name, value in self.headers.items()
        })
        plan = self.state.response_plan()
        if plan["drop"]:
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.connection.close()
            except OSError:
                pass
            return

        time.sleep(plan["delay_ms"] / 1000.0)
        status = 404 if self.path == plan["not_found_path"] else plan["status"]
        self._send_response(status, request_body_bytes, plan)

    def _body(self, status, request_body_bytes, target_size):
        payload = {
            "upstream": self.state.name,
            "status": status,
            "method": self.command,
            "path": self.path,
            "body_bytes": request_body_bytes,
            "headers": {
                "x-request-id": self.headers.get("X-Request-ID", ""),
                "x-bench-version": self.headers.get("X-Bench-Version", ""),
                "x-internal-debug": self.headers.get("X-Internal-Debug", ""),
            },
            "pad": "",
        }
        encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        payload["pad"] = "x" * max(0, target_size - len(encoded))
        return json.dumps(payload, separators=(",", ":")).encode("utf-8")

    def _send_response(self, status, request_body_bytes, plan):
        body = self._body(status, request_body_bytes, plan["body_size"])
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Connection", "keep-alive")
        if plan["chunked"]:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()

        try:
            if not plan["chunked"]:
                self.wfile.write(body)
            else:
                midpoint = max(1, len(body) // 2)
                for part in (body[:midpoint], body[midpoint:]):
                    self.wfile.write(f"{len(part):X}\r\n".encode("ascii"))
                    self.wfile.write(part)
                    self.wfile.write(b"\r\n")
                    self.wfile.flush()
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            return
        self.state.record_response(status)

    def _websocket(self):
        key = self.headers.get("Sec-WebSocket-Key", "").strip()
        if not key:
            self._send_json(400, {"error": "missing Sec-WebSocket-Key"})
            return
        accept = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
        ).digest()).decode("ascii")
        self.state.record_request(self.command, self.path, {
            name.lower(): value for name, value in self.headers.items()
        })
        self.state.record_ws_session()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.wfile.flush()
        try:
            while True:
                opcode, payload = self._read_ws_frame()
                if opcode is None:
                    return
                if opcode == 0x8:
                    self._write_ws_frame(0x8, payload)
                    return
                if opcode == 0x9:
                    self._write_ws_frame(0xA, payload)
                    continue
                if opcode in (0x1, 0x2):
                    self.state.record_ws_frame()
                    self._write_ws_frame(opcode, payload)
        except (BrokenPipeError, ConnectionResetError, OSError):
            return

    def _read_ws_frame(self):
        head = self.rfile.read(2)
        if len(head) != 2:
            return None, b""
        opcode = head[0] & 0x0F
        masked = bool(head[1] & 0x80)
        length = head[1] & 0x7F
        if length == 126:
            extended = self.rfile.read(2)
            if len(extended) != 2:
                return None, b""
            length = int.from_bytes(extended, "big")
        elif length == 127:
            extended = self.rfile.read(8)
            if len(extended) != 8:
                return None, b""
            length = int.from_bytes(extended, "big")
        if length > 1024 * 1024:
            raise OSError("WebSocket frame too large")
        mask = self.rfile.read(4) if masked else b""
        if masked and len(mask) != 4:
            return None, b""
        payload = self.rfile.read(length)
        if len(payload) != length:
            return None, b""
        if masked:
            payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        return opcode, payload

    def _write_ws_frame(self, opcode, payload):
        length = len(payload)
        if length < 126:
            head = bytes((0x80 | opcode, length))
        elif length <= 0xFFFF:
            head = bytes((0x80 | opcode, 126)) + length.to_bytes(2, "big")
        else:
            head = bytes((0x80 | opcode, 127)) + length.to_bytes(8, "big")
        self.wfile.write(head + payload)
        self.wfile.flush()

    def _send_json(self, status, value):
        body = (json.dumps(value, separators=(",", ":")) + "\n").encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _control(self):
        content_length = self._content_length()
        if content_length is None:
            return
        try:
            payload = json.loads(self.rfile.read(content_length).decode("utf-8"))
            if not isinstance(payload, dict):
                raise ValueError("control payload must be a JSON object")
            self.state.configure(payload)
        except (UnicodeDecodeError, ValueError, json.JSONDecodeError) as error:
            self._send_json(400, {"error": str(error)})
            return
        self._send_json(200, self.state.snapshot())

    def log_message(self, *_args):
        pass


class RichThreadingHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 1024


def probability(value):
    value = float(value)
    if not 0.0 <= value <= 1.0:
        raise argparse.ArgumentTypeError("must be between 0 and 1")
    return value


def body_sizes(value):
    try:
        sizes = tuple(int(item) for item in value.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be comma-separated byte counts") from error
    if not sizes or any(size < 100 for size in sizes):
        raise argparse.ArgumentTypeError("each body size must be at least 100")
    return sizes


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--name", default="soak-mock")
    parser.add_argument("--healthy", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--error-probability", type=probability, default=0.0)
    parser.add_argument("--error-burst", type=int, default=1)
    parser.add_argument("--drop-probability", type=probability, default=0.0)
    parser.add_argument("--chunked-probability", type=probability, default=0.10)
    parser.add_argument("--slow-probability", type=probability, default=0.0)
    parser.add_argument("--slow-delay-ms", type=float, default=2000.0)
    parser.add_argument("--delay-p50-ms", type=float, default=15.0)
    parser.add_argument("--delay-p99-ms", type=float, default=40.0)
    parser.add_argument("--max-normal-delay-ms", type=float, default=100.0)
    parser.add_argument("--body-sizes", type=body_sizes, default=(100, 1024, 50 * 1024),
                        help="comma-separated response body sizes in bytes")
    parser.add_argument("--not-found-path", default="/other")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.port < 1 or args.port > 65535:
        raise SystemExit("--port must be between 1 and 65535")
    if not args.name or any(ch.isspace() for ch in args.name):
        raise SystemExit("--name must be a non-empty token")
    if args.error_burst < 1:
        raise SystemExit("--error-burst must be >= 1")
    if args.delay_p99_ms < args.delay_p50_ms:
        raise SystemExit("--delay-p99-ms must be >= --delay-p50-ms")
    if args.max_normal_delay_ms < args.delay_p50_ms:
        raise SystemExit("--max-normal-delay-ms must be >= --delay-p50-ms")

    server = RichThreadingHTTPServer((args.host, args.port), RichHandler)
    server.state = MockState(args)
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
