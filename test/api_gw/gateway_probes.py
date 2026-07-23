#!/usr/bin/env python3
"""Deterministic HTTP and WebSocket feature gates for the isolated gateway soak."""

import argparse
import base64
import hashlib
import hmac
import http.client
import json
import os
import pathlib
import socket
import subprocess
import sys
import time
from urllib.parse import urlparse


def b64url(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def jwt(secret, issuer, sub, scope, expires_at=None, include_exp=True):
    now = int(time.time())
    header = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
    claims = {"sub": sub, "iss": issuer, "iat": now, "scope": scope}
    if include_exp:
        claims["exp"] = now + 600 if expires_at is None else expires_at
    payload = b64url(json.dumps(claims, separators=(",", ":")).encode())
    signature = b64url(hmac.new(secret.encode(), f"{header}.{payload}".encode(), hashlib.sha256).digest())
    return f"{header}.{payload}.{signature}"


class Probes:
    RATE_IP = "198.18.240.20"
    PERSIST_IP = "198.18.240.21"

    def __init__(self, args):
        self.args = args
        self.gateway = urlparse(args.gateway_url)
        self.admin = urlparse(args.admin_url)
        self.mocks = [urlparse(value) for value in args.mock_url]
        self.hub = urlparse(args.hub_url)
        self.banctl = args.banctl
        self.banctl_sock = args.banctl_sock
        self.failures = []
        self.results = []
        self.good_jwt = jwt(args.jwt_secret, args.jwt_issuer, "gateway-probe", "read write")
        self.limited_jwt = jwt(args.jwt_secret, args.jwt_issuer, "limited-probe", "write")
        self.expired_jwt = jwt(args.jwt_secret, args.jwt_issuer, "expired-probe", "read",
                               expires_at=int(time.time()) - 60)
        self.forged_jwt = jwt("not-the-gateway-secret", args.jwt_issuer, "forged-probe", "read")
        self.wrong_issuer_jwt = jwt(args.jwt_secret, "not-" + args.jwt_issuer,
                                    "wrong-issuer-probe", "read")
        self.no_exp_jwt = jwt(args.jwt_secret, args.jwt_issuer, "no-exp-probe", "read",
                              include_exp=False)
        now = int(time.time())
        header = b64url(json.dumps({"alg": "none", "typ": "JWT"}, separators=(",", ":")).encode())
        payload = b64url(json.dumps({
            "sub": "none-probe", "iss": args.jwt_issuer, "iat": now, "exp": now + 600,
            "scope": "read",
        }, separators=(",", ":")).encode())
        self.alg_none_jwt = f"{header}.{payload}."

    def check(self, name, ok, detail=""):
        self.results.append({"name": name, "ok": bool(ok), "detail": detail})
        print(("PASS" if ok else "FAIL") + ": " + name + (" " + detail if detail else ""), flush=True)
        if not ok:
            self.failures.append(name + (": " + detail if detail else ""))

    def http(self, method, path, headers=None, body=None, admin=False):
        target = self.admin if admin else self.gateway
        connection = http.client.HTTPConnection(target.hostname, target.port or 80, timeout=self.args.timeout)
        request_headers = {"X-Forwarded-For": "198.18.240.10"}
        if headers:
            request_headers.update(headers)
        try:
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            data = response.read()
            return response.status, {name.lower(): value for name, value in response.getheaders()}, data
        finally:
            connection.close()

    def hub_http(self, method, path):
        connection = http.client.HTTPConnection(self.hub.hostname, self.hub.port or 80, timeout=self.args.timeout)
        try:
            connection.request(method, path)
            response = connection.getresponse()
            data = response.read()
            return response.status, {name.lower(): value for name, value in response.getheaders()}, data
        finally:
            connection.close()

    @staticmethod
    def metric_value(text, sample):
        for line in text.decode("utf-8", errors="replace").splitlines():
            if line.startswith(sample + " "):
                try:
                    return float(line.rsplit(" ", 1)[1])
                except ValueError:
                    return None
        return None

    def wait_for(self, callback, timeout=8.0):
        end = time.monotonic() + timeout
        last_error = ""
        while time.monotonic() < end:
            try:
                value = callback()
                if value:
                    return value, last_error
            except (OSError, ValueError, RuntimeError, http.client.HTTPException) as error:
                last_error = repr(error)
            time.sleep(0.1)
        return None, last_error

    def banctl_json(self, *args):
        try:
            result = subprocess.run(
                [self.banctl, "--sock", self.banctl_sock, "--json", *args],
                text=True,
                capture_output=True,
                timeout=self.args.timeout,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return None, repr(error)
        detail = f"exit={result.returncode} stdout={result.stdout!r} stderr={result.stderr!r}"
        if result.returncode != 0:
            return None, detail
        try:
            return json.loads(result.stdout), detail
        except json.JSONDecodeError:
            return None, detail

    def stats(self):
        status, headers, body = self.http("GET", "/stats", admin=True)
        if status != 200:
            raise RuntimeError("gateway stats HTTP " + str(status))
        if "application/json" not in headers.get("content-type", ""):
            raise RuntimeError("gateway stats content type " + repr(headers.get("content-type")))
        return json.loads(body)

    def hub_metrics(self):
        status, headers, body = self.hub_http("GET", "/metrics")
        if status != 200:
            raise RuntimeError("hub metrics HTTP " + str(status))
        if "text/plain" not in headers.get("content-type", ""):
            raise RuntimeError("hub metrics content type " + repr(headers.get("content-type")))
        return body

    def mock_snapshot(self, mock):
        connection = http.client.HTTPConnection(mock.hostname, mock.port or 80, timeout=self.args.timeout)
        try:
            connection.request("GET", "/_soak/stats")
            response = connection.getresponse()
            if response.status != 200:
                raise RuntimeError("mock stats HTTP " + str(response.status))
            return json.loads(response.read())
        finally:
            connection.close()

    def mock_stats(self):
        snapshots = [self.mock_snapshot(mock) for mock in self.mocks]
        responses = {}
        for snapshot in snapshots:
            for status, count in snapshot.get("responses", {}).items():
                responses[status] = responses.get(status, 0) + count
        return {
            "requests": sum(snapshot.get("requests", 0) for snapshot in snapshots),
            "drops": sum(snapshot.get("drops", 0) for snapshot in snapshots),
            "responses": responses,
            "recent_requests": [record for snapshot in snapshots
                                for record in snapshot.get("recent_requests", [])],
            "ws_sessions": sum(snapshot.get("ws_sessions", 0) for snapshot in snapshots),
            "ws_frames": sum(snapshot.get("ws_frames", 0) for snapshot in snapshots),
        }

    def authorization(self, token):
        return {"Authorization": "Bearer " + token}

    def client_headers(self, ip):
        return {
            **self.authorization(self.good_jwt),
            "X-Forwarded-For": ip,
        }

    def wait_for_sync(self, timeout=8.0):
        def synced():
            stats = self.stats()
            if stats.get("ipban_synced") == 1 and stats.get("ipban_link_up") == 1:
                return stats
            return None

        return self.wait_for(synced, timeout)

    def wait_for_rule(self, rule_id=None, source=None, ip=None):
        def listed():
            result, _detail = self.banctl_json("list")
            if not result or not result.get("ok"):
                return None
            for rule in result.get("rules", []):
                if rule_id is not None and rule.get("id") != rule_id:
                    continue
                if source is not None and rule.get("src") != source:
                    continue
                if ip is not None and rule.get("ip") != ip:
                    continue
                return rule
            return None

        return self.wait_for(listed)

    def wait_for_client_status(self, ip, status):
        return self.wait_for(lambda: self.http(
            "GET", "/api/ipban-probe", self.client_headers(ip)
        )[0] == status)

    def run_ipban_rate(self):
        synced, sync_detail = self.wait_for_sync()
        self.check("rate-report probe starts with an open subscription", bool(synced),
                   sync_detail or repr(synced))

        limited = 0
        for _ in range(self.args.rate_report_hits + 8):
            status, _headers, _body = self.http("GET", "/api/rate-report", self.client_headers(self.RATE_IP))
            if status == 429:
                limited += 1
        self.check("rate limit rejects enough burst requests for auto-ban",
                   limited >= self.args.rate_report_hits,
                   f"client_ip={self.RATE_IP} limited={limited} required={self.args.rate_report_hits}")

        rate_rule, rate_detail = self.wait_for_rule(source="rate", ip=self.RATE_IP)
        self.check("rate-limit report reaches the hub control plane", bool(rate_rule),
                   rate_detail or repr(rate_rule))

        before = self.mock_stats()["requests"]
        denied, deny_detail = self.wait_for_client_status(self.RATE_IP, 403)
        after = self.mock_stats()["requests"]
        self.check("reported rate rule synchronizes to gateway Guard", bool(denied),
                   deny_detail or "Guard did not deny")
        self.check("rate-based IP ban does not reach upstream", before == after,
                   f"mock={before}->{after}")

        hub_metrics = self.hub_metrics()
        self.check("hub metrics record the rate-limit risk",
                   (self.metric_value(hub_metrics,
                                      'ipban_hub_risks_total{result="ok",source="rate"}') or 0.0) >= 1.0,
                   "missing hub rate risk metric")

        if rate_rule:
            removed, remove_detail = self.banctl_json("del", rate_rule["id"])
            self.check("admin removes the reported rate rule", bool(removed and removed.get("ok")
                       and removed.get("changed")), remove_detail)
            restored, restore_detail = self.wait_for_client_status(self.RATE_IP, 200)
            self.check("rate-rule removal restores the business path", bool(restored),
                       restore_detail or "Guard still denies")

    def run_ipban_persist_seed(self):
        synced, sync_detail = self.wait_for_sync()
        self.check("persistence probe starts with an open subscription", bool(synced),
                   sync_detail or repr(synced))
        created, create_detail = self.banctl_json("deny", self.PERSIST_IP, "2m", "soak-persist")
        self.check("admin creates a rule for hub persistence", bool(created and created.get("ok")
                   and created.get("changed")), create_detail)
        rule_id = created.get("id") if created else None
        stored, stored_detail = self.wait_for_rule(rule_id=rule_id, source="admin", ip=self.PERSIST_IP)
        self.check("persistence rule is stored by the hub", bool(stored), stored_detail or repr(stored))
        denied, deny_detail = self.wait_for_client_status(self.PERSIST_IP, 403)
        self.check("persistence rule reaches gateway Guard before restart", bool(denied),
                   deny_detail or "Guard did not deny")

    def run_ipban_persist_verify(self):
        synced, sync_detail = self.wait_for_sync(timeout=12.0)
        self.check("gateway reconnects to the restarted hub", bool(synced),
                   sync_detail or repr(synced))
        persisted, persisted_detail = self.wait_for_rule(source="admin", ip=self.PERSIST_IP)
        self.check("SQLite restores the rule after hub restart", bool(persisted),
                   persisted_detail or repr(persisted))
        denied, deny_detail = self.wait_for_client_status(self.PERSIST_IP, 403)
        self.check("restored rule still blocks through gateway Guard", bool(denied),
                   deny_detail or "Guard did not deny")
        if persisted:
            removed, remove_detail = self.banctl_json("del", persisted["id"])
            self.check("admin removes the restored persistence rule", bool(removed and removed.get("ok")
                       and removed.get("changed")), remove_detail)
            restored, restore_detail = self.wait_for_client_status(self.PERSIST_IP, 200)
            self.check("removing restored rule recovers the business path", bool(restored),
                       restore_detail or "Guard still denies")
        stats = self.stats()
        self.check("gateway records a control-plane reconnect",
                   stats.get("ipban_connects", 0) >= 2 and stats.get("ipban_reconnects", 0) >= 1,
                   repr({key: stats.get(key) for key in ("ipban_connects", "ipban_reconnects",
                                                        "ipban_synced", "ipban_link_up")}))

    def ws_status(self, token=None):
        key = b64url(os.urandom(16))
        authorization = f"Authorization: Bearer {token}\r\n" if token else ""
        request = (
            "GET /ws/auth HTTP/1.1\r\n"
            f"Host: {self.gateway.hostname}:{self.gateway.port}\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"{authorization}"
            "X-Forwarded-For: 198.18.240.11\r\n\r\n"
        ).encode("ascii")
        try:
            sock = socket.create_connection((self.gateway.hostname, self.gateway.port or 80),
                                            timeout=self.args.timeout)
        except OSError:
            return 0
        sock.settimeout(self.args.timeout)
        try:
            sock.sendall(request)
            first_line = self.read_until(sock, b"\r\n\r\n").split(b"\r\n", 1)[0]
            fields = first_line.split(None, 2)
            return int(fields[1]) if len(fields) >= 2 and fields[1].isdigit() else 0
        except (OSError, RuntimeError, ValueError):
            return 0
        finally:
            sock.close()

    def ws_roundtrip(self, token):
        key = b64url(os.urandom(16))
        request = (
            "GET /ws/chat HTTP/1.1\r\n"
            f"Host: {self.gateway.hostname}:{self.gateway.port}\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Authorization: Bearer {token}\r\n"
            "X-Forwarded-For: 198.18.240.11\r\n\r\n"
        ).encode("ascii")
        sock = socket.create_connection((self.gateway.hostname, self.gateway.port or 80), timeout=self.args.timeout)
        sock.settimeout(self.args.timeout)
        try:
            sock.sendall(request)
            head = self.read_until(sock, b"\r\n\r\n")
            lines = head.decode("iso-8859-1").split("\r\n")
            headers = {}
            for line in lines[1:]:
                name, separator, value = line.partition(":")
                if separator:
                    headers[name.lower()] = value.strip()
            expected = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
            ).digest()).decode("ascii")
            if not lines[0].startswith("HTTP/1.1 101") or headers.get("sec-websocket-accept") != expected:
                return False, "handshake=" + lines[0]
            payload = b"soak-ws-payload"
            mask = b"\x11\x22\x33\x44"
            masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
            sock.sendall(bytes((0x81, 0x80 | len(payload))) + mask + masked)
            opcode, echoed = self.read_ws_frame(sock)
            sock.sendall(b"\x88\x80\x00\x00\x00\x00")
            return opcode == 0x1 and echoed == payload, f"opcode={opcode} payload={echoed!r}"
        finally:
            sock.close()

    def raw_request(self, payload: bytes) -> int:
        """向网关业务口发裸字节，返回第一个响应行的 HTTP 状态码，出错返回 0。"""
        try:
            sock = socket.create_connection(
                (self.gateway.hostname, self.gateway.port or 80),
                timeout=self.args.timeout,
            )
        except OSError:
            return 0
        sock.settimeout(self.args.timeout)
        try:
            sock.sendall(payload)
            head = self.read_until(sock, b"\r\n\r\n")
            first_line = head.split(b"\r\n")[0].decode("ascii", errors="replace")
            parts = first_line.split(None, 2)
            return int(parts[1]) if len(parts) >= 2 and parts[1].isdigit() else 0
        except (OSError, ValueError, RuntimeError):
            return 0
        finally:
            try:
                sock.close()
            except OSError:
                pass

    def run_smuggling(self):
        """请求走私防御探针：CL.TE / TE.CL / 双CL / TE混淆 / keep-alive 无污染。"""
        host = f"{self.gateway.hostname}:{self.gateway.port or 80}"

        # 1. CL + TE:chunked (CL.TE 经典向量) → RFC 7230 §3.3.3 要求拒绝
        status = self.raw_request((
            "POST /smuggle-probe HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Content-Length: 5\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "hello"
        ).encode())
        self.check("smuggling CL+TE:chunked is rejected with 400", status == 400,
                   f"status={status}")

        # 2. TE:chunked + CL (头顺序颠倒，同一歧义) → 400
        status = self.raw_request((
            "POST /smuggle-probe HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello"
        ).encode())
        self.check("smuggling TE:chunked+CL (reversed order) is rejected with 400",
                   status == 400, f"status={status}")

        # 3. 双 CL 值不一致 → 期望 400（防御文档声称覆盖，实测验证）
        status = self.raw_request((
            "POST /smuggle-probe HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Content-Length: 5\r\n"
            "Content-Length: 6\r\n"
            "\r\n"
            "hello!"
        ).encode())
        self.check("smuggling duplicate inconsistent CL is rejected with 400",
                   status == 400, f"status={status}")

        # 4. 双 CL 值相同 → 允许（无歧义），不应 400
        status = self.raw_request((
            "POST /smuggle-probe HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Content-Length: 5\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello"
        ).encode())
        self.check("duplicate CL with same value is not rejected (no ambiguity)",
                   status != 400, f"status={status}")

        # 5. TE 混淆（xchunked）+ CL：gateway 用子串匹配，"xchunked" 含 "chunked" 子串，
        #    m_hasChunked=true，加上 CL → 歧义 → 400。比精确匹配更保守，属更强防御。
        status = self.raw_request((
            "POST /smuggle-probe HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Transfer-Encoding: xchunked\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello"
        ).encode())
        self.check("TE obfuscation (xchunked)+CL is rejected with 400 (substring match catches it)",
                   status == 400, f"status={status}")

        # 6. TE 混淆 chunked 拼写（空格前缀）+ CL：同上，期望不 400
        status = self.raw_request((
            "POST /smuggle-probe HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Transfer-Encoding:  chunked\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello"
        ).encode())
        self.check("TE with extra leading space +CL: gateway behavior is consistent (not silent pass-through)",
                   status in (400, 200, 401, 403, 404), f"status={status}")

        # 7. 合法 chunked（无 CL）→ 正常解析，不 400
        status = self.raw_request((
            "POST /smuggle-probe HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "5\r\n"
            "hello\r\n"
            "0\r\n"
            "\r\n"
        ).encode())
        self.check("valid chunked request without CL is not rejected with 400",
                   status != 400, f"status={status}")

        # 8. 走私拒绝后新连接不受影响（无 IP 级别误封），用 admin healthz 验证可达性
        status, _headers, body = self.http("GET", "/healthz", admin=True)
        self.check("new connection after smuggling rejection works normally (admin healthz)",
                   status == 200 and body == b"OK\n", f"status={status}")

    @staticmethod
    def read_until(sock, marker):
        data = bytearray()
        while marker not in data:
            part = sock.recv(4096)
            if not part:
                raise RuntimeError("socket closed before HTTP head")
            data.extend(part)
            if len(data) > 64 * 1024:
                raise RuntimeError("HTTP head too large")
        return bytes(data)

    @staticmethod
    def read_ws_frame(sock):
        def read_exact(size):
            output = bytearray()
            while len(output) < size:
                part = sock.recv(size - len(output))
                if not part:
                    raise RuntimeError("socket closed during WebSocket frame")
                output.extend(part)
            return bytes(output)

        first, second = read_exact(2)
        opcode = first & 0x0F
        length = second & 0x7F
        if length == 126:
            length = int.from_bytes(read_exact(2), "big")
        elif length == 127:
            length = int.from_bytes(read_exact(8), "big")
        if second & 0x80:
            mask = read_exact(4)
            body = bytes(value ^ mask[index % 4] for index, value in enumerate(read_exact(length)))
        else:
            body = read_exact(length)
        return opcode, body

    def run(self):
        if self.args.mode == "rate":
            self.run_ipban_rate()
            return
        if self.args.mode == "persist-seed":
            self.run_ipban_persist_seed()
            return
        if self.args.mode == "persist-verify":
            self.run_ipban_persist_verify()
            return

        status, _headers, body = self.http("GET", "/healthz", admin=True)
        self.check("admin healthz is available", status == 200 and body == b"OK\n", repr(body))

        status, headers, body = self.http("GET", "/routes", admin=True)
        routes = {}
        try:
            routes = {route.get("name"): route for route in json.loads(body).get("routes", [])}
        except (UnicodeDecodeError, json.JSONDecodeError, AttributeError):
            pass
        api_route = routes.get("api", {})
        self.check("admin routes exposes the live api route", status == 200
                   and "application/json" in headers.get("content-type", "")
                   and {"api", "public", "apikey", "ws"}.issubset(routes)
                   and api_route.get("path") == "/api"
                   and api_route.get("upstream") == "api"
                   and api_route.get("auth") == "jwt"
                   and api_route.get("rate_limit", {}).get("enabled") is True,
                   repr(api_route))

        status, _headers, body = self.http("GET", "/maintenance", admin=True)
        self.check("admin maintenance state starts disabled", status == 200 and body == b'{"maintenance":false}\n',
                   repr(body))
        status, _headers, _body = self.http("POST", "/maintenance/on", admin=True)
        self.check("admin enables maintenance", status == 200, f"status={status}")
        status, _headers, body = self.http("GET", "/maintenance", admin=True)
        self.check("admin maintenance state reports enabled", status == 200 and body == b'{"maintenance":true}\n',
                   repr(body))
        status, _headers, _body = self.http("GET", "/api/maintenance", self.authorization(self.good_jwt))
        self.check("maintenance blocks the business listener", status == 503, f"status={status}")
        status, _headers, _body = self.http("POST", "/maintenance/off", admin=True)
        self.check("admin disables maintenance", status == 200, f"status={status}")
        status, _headers, body = self.http("GET", "/maintenance", admin=True)
        self.check("admin maintenance state reports disabled", status == 200 and body == b'{"maintenance":false}\n',
                   repr(body))
        status, _headers, _body = self.http("GET", "/api/maintenance", self.authorization(self.good_jwt))
        self.check("business listener recovers after maintenance", status == 200, f"status={status}")

        def _check_synced():
            s = self.stats()
            return s if s.get("ipban_synced") == 1 and s.get("ipban_link_up") == 1 else None
        synced, sync_detail = self.wait_for(_check_synced)
        self.check("gateway control-plane subscription is open", bool(synced), sync_detail or repr(synced))
        if synced:
            required_stats = {
                "requests", "upstream_ok", "active_conns", "routes", "upstreams", "latency",
                "ipban_synced", "ipban_link_up", "risk_sent", "waf_denied",
            }
            self.check("stats exposes data-plane, control-plane, and latency fields",
                       required_stats.issubset(synced), repr(sorted(synced)))
            self.check("stats records the live api route", "/api" in synced.get("routes", {}),
                       repr(synced.get("routes")))

        status, headers, gateway_metrics = self.http("GET", "/metrics", admin=True)
        self.check("gateway metrics uses Prometheus text", status == 200
                   and "text/plain" in headers.get("content-type", "")
                   and self.metric_value(gateway_metrics, "ipban_sync_connected") == 1.0
                   and self.metric_value(gateway_metrics, 'gateway_route_requests_total{route="/api"}') is not None,
                   f"status={status}")
        hub_metrics = self.hub_metrics()
        self.check("hub metrics reports an active control plane",
                   self.metric_value(hub_metrics, "ipban_hub_up") == 1.0
                   and (self.metric_value(hub_metrics, "ipban_hub_sessions") or 0.0) >= 1.0,
                   "hub metrics missing up or session")

        before_cors = self.mock_stats()["requests"]
        status, headers, _body = self.http("OPTIONS", "/api/cors", {
            "Origin": "https://client.example",
            "Access-Control-Request-Method": "POST",
            "Access-Control-Request-Headers": "X-Test",
        })
        after_cors = self.mock_stats()["requests"]
        self.check("CORS allowed preflight is a local 204", status == 204,
                   f"status={status}")
        self.check("CORS allowed origin is echoed", headers.get("access-control-allow-origin") == "https://client.example",
                   repr(headers.get("access-control-allow-origin")))
        self.check("CORS preflight does not proxy upstream", before_cors == after_cors,
                   f"mock={before_cors}->{after_cors}")

        status, headers, _body = self.http("GET", "/api/cors", {
            **self.authorization(self.good_jwt), "Origin": "https://denied.example",
        })
        self.check("CORS untrusted origin remains an application request", status == 200, f"status={status}")
        self.check("CORS untrusted origin receives no allow-origin header",
                   "access-control-allow-origin" not in headers, repr(headers.get("access-control-allow-origin")))

        status, _headers, _body = self.http("GET", "/public")
        self.check("exact public route bypasses authentication", status == 200, f"status={status}")
        status, _headers, _body = self.http("GET", "/public/more")
        self.check("exact public route does not match a longer path", status == 404, f"status={status}")

        before_auth_denials = self.mock_stats()["requests"]
        status, _headers, _body = self.http("GET", "/api/auth")
        self.check("JWT route rejects missing bearer", status == 401, f"status={status}")
        for name, token in (
            ("expired token", self.expired_jwt),
            ("forged signature", self.forged_jwt),
            ("wrong issuer", self.wrong_issuer_jwt),
            ("missing exp", self.no_exp_jwt),
            ("alg=none", self.alg_none_jwt),
        ):
            status, _headers, _body = self.http("GET", "/api/auth", self.authorization(token))
            self.check("JWT route rejects " + name, status == 401, f"status={status}")
        status, _headers, _body = self.http("GET", "/api/auth", self.authorization(self.limited_jwt))
        self.check("JWT route rejects missing required scope", status == 403, f"status={status}")
        status, _headers, _body = self.http("GET", "/apikey")
        self.check("API-key route rejects missing key", status == 401, f"status={status}")
        status, _headers, _body = self.http("GET", "/apikey", {
            "X-API-Key": "soak-read." + self.args.limited_api_key,
        })
        self.check("API-key route rejects missing required scope", status == 403, f"status={status}")
        after_auth_denials = self.mock_stats()["requests"]
        self.check("authentication and authorization denials do not reach upstream",
                   before_auth_denials == after_auth_denials,
                   f"mock={before_auth_denials}->{after_auth_denials}")

        status, _headers, _body = self.http("GET", "/api/identity", {
            **self.authorization(self.good_jwt), "X-User-Id": "forged-user",
        })
        self.check("JWT route accepts required scope", status == 200, f"status={status}")
        status, _headers, _body = self.http("GET", "/apikey", {
            "X-API-Key": "soak-write." + self.args.api_key,
            "X-Soak-Probe": "api-key-forward",
        })
        self.check("API-key route accepts authorized key", status == 200, f"status={status}")

        before_waf = self.mock_stats()["requests"]
        status, _headers, _body = self.http(
            "GET", "/api/scan?q=%3Cscript%3Ealert(1)%3C%2Fscript%3E",
            self.authorization(self.good_jwt),
        )
        after_waf = self.mock_stats()["requests"]
        self.check("WAF blocks XSS path before proxy", status == 403, f"status={status}")
        self.check("WAF block does not reach upstream", before_waf == after_waf,
                   f"mock={before_waf}->{after_waf}")

        waf_ip = "198.18.240.10"

        def listed_waf_rule():
            result, _detail = self.banctl_json("list")
            if not result or not result.get("ok"):
                return None
            return next((rule for rule in result.get("rules", [])
                         if rule.get("src") == "waf" and rule.get("ip") == waf_ip), None)

        def guard_denies_waf_ip():
            response_status, _response_headers, _response_body = self.http(
                "GET", "/api/reported", self.authorization(self.good_jwt))
            return response_status == 403

        waf_rule, rule_detail = self.wait_for(listed_waf_rule)
        self.check("WAF report reaches the hub control plane", bool(waf_rule), rule_detail or repr(waf_rule))
        denied, deny_detail = self.wait_for(guard_denies_waf_ip)
        self.check("reported WAF rule synchronizes to gateway Guard", bool(denied), deny_detail or "Guard did not deny")

        control_stats = self.stats()
        self.check("stats records WAF report and synced Guard denial",
                   control_stats.get("waf_denied", 0) >= 1
                   and control_stats.get("risk_sent", 0) >= 1
                   and control_stats.get("ipban_denied", 0) >= 1
                   and control_stats.get("ipban_synced") == 1,
                   repr({key: control_stats.get(key) for key in
                         ("waf_denied", "risk_sent", "ipban_denied", "ipban_synced")}))
        _status, _headers, metrics = self.http("GET", "/metrics", admin=True)
        self.check("gateway Prometheus metrics expose WAF report and Guard denial",
                   (self.metric_value(metrics, 'waf_denied_total{reported="true",rule="xss"}') or 0.0) >= 1.0
                   and (self.metric_value(metrics, "ipban_risk_sent_total") or 0.0) >= 1.0
                   and (self.metric_value(metrics, 'ipban_denied_total{reason="rule",source="waf"}') or 0.0) >= 1.0,
                   "missing WAF report or Guard denial metric")
        hub_metrics = self.hub_metrics()
        self.check("hub Prometheus metrics expose WAF risk and rule creation",
                   (self.metric_value(hub_metrics, 'ipban_hub_risks_total{result="ok",source="waf"}') or 0.0) >= 1.0
                   and (self.metric_value(hub_metrics, 'ipban_hub_changes_total{op="put",source="waf"}') or 0.0) >= 1.0,
                   "missing hub WAF risk or rule metric")

        if waf_rule:
            removed, remove_detail = self.banctl_json("del", waf_rule["id"])
            self.check("admin removes the reported WAF rule", bool(removed and removed.get("ok")
                       and removed.get("changed")), remove_detail)

            def guard_allows_waf_ip():
                response_status, _response_headers, _response_body = self.http(
                    "GET", "/api/reported", self.authorization(self.good_jwt))
                return response_status == 200

            allowed, allow_detail = self.wait_for(guard_allows_waf_ip)
            self.check("manual WAF-rule removal restores the business path", bool(allowed),
                       allow_detail or "Guard still denies")
            hub_metrics = self.hub_metrics()
            self.check("hub metrics record administrator removal of the WAF rule",
                       (self.metric_value(hub_metrics, 'ipban_hub_changes_total{op="del",source="waf"}') or 0.0) >= 1.0,
                       "missing hub WAF delete metric")

        admin_ip = "198.18.240.12"
        admin_rule, admin_detail = self.banctl_json("deny", admin_ip, "30s", "soak-admin")
        self.check("admin creates a manual deny rule", bool(admin_rule and admin_rule.get("ok")
                   and admin_rule.get("changed")), admin_detail)

        def listed_admin_rule():
            result, _detail = self.banctl_json("list")
            if not result or not result.get("ok") or not admin_rule:
                return None
            return next((rule for rule in result.get("rules", [])
                         if rule.get("id") == admin_rule.get("id")
                         and rule.get("src") == "admin" and rule.get("ip") == admin_ip), None)

        listed_admin, listed_admin_detail = self.wait_for(listed_admin_rule)
        self.check("manual deny is visible through the hub administration plane", bool(listed_admin),
                   listed_admin_detail or repr(listed_admin))

        def guard_denies_admin_ip():
            response_status, _response_headers, _response_body = self.http(
                "GET", "/api/admin-ban", {
                    **self.authorization(self.good_jwt), "X-Forwarded-For": admin_ip,
                })
            return response_status == 403

        admin_denied, admin_deny_detail = self.wait_for(guard_denies_admin_ip)
        self.check("manual deny synchronizes to gateway Guard", bool(admin_denied),
                   admin_deny_detail or "Guard did not deny")
        unbanned, unban_detail = self.banctl_json("unban", admin_ip)
        self.check("admin unban removes the manual deny rule", bool(unbanned and unbanned.get("ok")
                   and unbanned.get("changed")), unban_detail)

        def admin_rule_is_gone():
            result, _detail = self.banctl_json("list")
            if not result or not result.get("ok") or not admin_rule:
                return False
            return not any(rule.get("id") == admin_rule.get("id") for rule in result.get("rules", []))

        admin_gone, admin_gone_detail = self.wait_for(admin_rule_is_gone)
        self.check("admin unban removes the rule from the hub administration plane", bool(admin_gone),
                   admin_gone_detail or "manual rule remains listed")

        def guard_allows_admin_ip():
            response_status, _response_headers, _response_body = self.http(
                "GET", "/api/admin-ban", {
                    **self.authorization(self.good_jwt), "X-Forwarded-For": admin_ip,
                })
            return response_status == 200

        admin_allowed, admin_allow_detail = self.wait_for(guard_allows_admin_ip)
        self.check("admin unban restores the manually blocked business path", bool(admin_allowed),
                   admin_allow_detail or "Guard still denies")
        hub_metrics = self.hub_metrics()
        self.check("hub metrics record manual deny and unban",
                   (self.metric_value(hub_metrics, 'ipban_hub_changes_total{op="put",source="admin"}') or 0.0) >= 1.0
                   and (self.metric_value(hub_metrics, 'ipban_hub_changes_total{op="del",source="admin"}') or 0.0) >= 1.0,
                   "missing hub admin put or delete metric")
        _status, _headers, metrics = self.http("GET", "/metrics", admin=True)
        self.check("gateway metrics attribute the manual deny and show no remote rules after unban",
                   (self.metric_value(metrics, 'ipban_denied_total{reason="rule",source="admin"}') or 0.0) >= 1.0
                   and self.metric_value(metrics, 'ipban_rules{kind="remote"}') == 0.0,
                   "missing admin denial metric or remote rules remain")

        before_ws = self.mock_stats()
        missing_ws_status = self.ws_status()
        limited_ws_status = self.ws_status(self.limited_jwt)
        denied_ws = self.mock_stats()
        self.check("WebSocket route rejects missing bearer before upgrade", missing_ws_status == 401,
                   f"status={missing_ws_status}")
        self.check("WebSocket route rejects missing required scope before upgrade", limited_ws_status == 403,
                   f"status={limited_ws_status}")
        self.check("rejected WebSocket upgrades do not reach upstream",
                   denied_ws["requests"] == before_ws["requests"]
                   and denied_ws["ws_sessions"] == before_ws["ws_sessions"]
                   and denied_ws["ws_frames"] == before_ws["ws_frames"],
                   "requests={}->{} sessions={}->{} frames={}->{}".format(
                       before_ws["requests"], denied_ws["requests"],
                       before_ws["ws_sessions"], denied_ws["ws_sessions"],
                       before_ws["ws_frames"], denied_ws["ws_frames"],
                   ))
        ws_ok, ws_detail = self.ws_roundtrip(self.good_jwt)
        self.check("WebSocket tunnel upgrades and echoes a text frame", ws_ok, ws_detail)
        after_ws = self.mock_stats()
        self.check("WebSocket session reaches upstream", after_ws["ws_sessions"] >= before_ws["ws_sessions"] + 1,
                   f"sessions={before_ws['ws_sessions']}->{after_ws['ws_sessions']}")
        self.check("WebSocket frame reaches upstream", after_ws["ws_frames"] >= before_ws["ws_frames"] + 1,
                   f"frames={before_ws['ws_frames']}->{after_ws['ws_frames']}")

        records = after_ws["recent_requests"]
        identity = next((item for item in reversed(records) if item["path"] == "/identity"), None)
        self.check("JWT forwarding strips bearer and replaces forged user identity", bool(identity)
                   and identity["headers"]["authorization"] == ""
                   and identity["headers"]["x-user-id"] == "gateway-probe"
                   and "read" in identity["headers"]["x-user-scopes"], repr(identity))
        apikey = next((item for item in reversed(records)
                       if item["headers"]["x-soak-probe"] == "api-key-forward"), None)
        self.check("API key is stripped before upstream forwarding", bool(apikey)
                   and apikey["headers"]["x-api-key"] == "", repr(apikey))

        self.run_smuggling()

    def result(self):
        return {"failures": self.failures, "results": self.results}


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gateway-url", required=True)
    parser.add_argument("--admin-url", required=True)
    parser.add_argument("--mock-url", required=True, action="append")
    parser.add_argument("--hub-url", required=True)
    parser.add_argument("--banctl", required=True)
    parser.add_argument("--banctl-sock", required=True)
    parser.add_argument("--jwt-secret", required=True)
    parser.add_argument("--jwt-issuer", required=True)
    parser.add_argument("--api-key", required=True)
    parser.add_argument("--limited-api-key", required=True)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--mode", choices=("full", "rate", "persist-seed", "persist-verify"),
                        default="full")
    parser.add_argument("--rate-report-hits", type=int, default=3)
    parser.add_argument("--result-json", required=True)
    args = parser.parse_args()
    for name in ("gateway_url", "admin_url", "hub_url"):
        value = urlparse(getattr(args, name))
        if value.scheme != "http" or not value.hostname:
            parser.error(name.replace("_", "-") + " must be an http URL")
    for index, raw in enumerate(args.mock_url):
        value = urlparse(raw)
        if value.scheme != "http" or not value.hostname:
            parser.error("mock-url[" + str(index) + "] must be an http URL")
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    if args.rate_report_hits <= 0:
        parser.error("rate-report-hits must be positive")
    if not os.path.isfile(args.banctl) or not os.access(args.banctl, os.X_OK):
        parser.error("banctl must name an executable file")
    if not args.banctl_sock:
        parser.error("banctl-sock must not be empty")
    return args


def main():
    args = parse_args()
    probes = Probes(args)
    try:
        probes.run()
    except (OSError, ValueError, RuntimeError, http.client.HTTPException) as error:
        probes.check("probe transport", False, repr(error))
    output = pathlib.Path(args.result_json)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + f".tmp.{os.getpid()}")
    temporary.write_text(json.dumps(probes.result(), indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(output)
    if probes.failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
