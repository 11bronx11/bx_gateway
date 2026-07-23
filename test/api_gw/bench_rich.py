#!/usr/bin/env python3
"""Zero-dependency mixed-workload HTTP/1.1 soak client for Bronx gateway."""

import argparse
import base64
import collections
import hashlib
import hmac
import http.client
import json
import math
import os
import pathlib
import random
import socket
import threading
import time
import sys
from urllib.parse import urlparse


DEFAULT_WEIGHTS = {
    "normal": 60,
    "no_token": 15,
    "post": 10,
    "not_found": 8,
    "bad_token": 4,
    "disconnect": 3,
}

SCENARIO_LABELS = {
    "normal": "GET /api/users, jwt",
    "no_token": "GET /api/users, no token",
    "post": "POST /api/users, jwt",
    "not_found": "GET /api/other, jwt",
    "bad_token": "GET /api/users, bad jwt",
    "disconnect": "GET /api/users, disconnect",
}


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def make_hs256_token(payload, secret):
    header = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
    body = b64url(json.dumps(payload, separators=(",", ":")).encode())
    signing_input = f"{header}.{body}".encode("ascii")
    signature = b64url(hmac.new(secret.encode("utf-8"), signing_input, hashlib.sha256).digest())
    return f"{header}.{body}.{signature}"


def parse_weights(text):
    weights = {}
    for item in text.split(","):
        key, sep, raw = item.strip().partition("=")
        if not sep or key not in DEFAULT_WEIGHTS:
            raise argparse.ArgumentTypeError("weights must use " + ",".join(f"{name}=N" for name in DEFAULT_WEIGHTS))
        try:
            value = float(raw)
        except ValueError as error:
            raise argparse.ArgumentTypeError(f"bad weight for {key}") from error
        if value < 0:
            raise argparse.ArgumentTypeError("weights cannot be negative")
        weights[key] = value
    if set(weights) != set(DEFAULT_WEIGHTS) or not any(weights.values()):
        raise argparse.ArgumentTypeError("all six non-zero-total scenario weights are required")
    return weights


def parse_status_ratio(text):
    scenario, separator, remainder = text.partition(":")
    status_text, separator, ratio_text = remainder.partition(":")
    if not separator or scenario not in DEFAULT_WEIGHTS:
        raise argparse.ArgumentTypeError("status ratio must be scenario:status:ratio")
    try:
        status = int(status_text)
        ratio = float(ratio_text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("status ratio needs integer status and numeric ratio") from error
    if not 100 <= status <= 599 or not 0.0 <= ratio <= 1.0:
        raise argparse.ArgumentTypeError("status must be 100..599 and ratio must be 0..1")
    return scenario, status, ratio


def parse_min_attempts(text):
    scenario, separator, count_text = text.partition(":")
    if not separator or scenario not in DEFAULT_WEIGHTS:
        raise argparse.ArgumentTypeError("minimum attempts must be scenario:count")
    try:
        count = int(count_text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("minimum attempt count must be an integer") from error
    if count < 0:
        raise argparse.ArgumentTypeError("minimum attempt count cannot be negative")
    return scenario, count


def percentile(values, fraction):
    if not values:
        return 0.0
    index = max(0, math.ceil(len(values) * fraction) - 1)
    return values[index]


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.requests = collections.Counter()
        self.codes = {name: collections.Counter() for name in DEFAULT_WEIGHTS}
        self.errors = collections.Counter()
        self.latencies_ms = []

    def response(self, scenario, status, latency_ms):
        with self.lock:
            self.requests[scenario] += 1
            self.codes[scenario][status] += 1
            self.latencies_ms.append(latency_ms)

    def disconnected(self, scenario):
        with self.lock:
            self.requests[scenario] += 1
            self.codes[scenario]["sent"] += 1

    def error(self, scenario, detail):
        with self.lock:
            self.requests[scenario] += 1
            self.errors[detail] += 1

    def snapshot(self):
        with self.lock:
            requests = dict(self.requests)
            codes = {name: dict(counter) for name, counter in self.codes.items()}
            errors = dict(self.errors)
            latencies = sorted(self.latencies_ms)
        return requests, codes, errors, latencies


class Pacer:
    """Issues shared request slots at a fixed global rate."""

    def __init__(self, requests_per_second):
        self.interval = 1.0 / requests_per_second
        self.lock = threading.Lock()
        self.next_slot = time.monotonic()

    def wait(self, stop, deadline):
        with self.lock:
            slot = self.next_slot
            self.next_slot += self.interval
        while not stop.is_set():
            remaining = slot - time.monotonic()
            if remaining <= 0:
                return time.monotonic() < deadline
            if stop.wait(min(remaining, 0.1)):
                return False
        return False


class Workload:
    def __init__(self, args):
        self.host = args.host
        self.port = args.port
        self.timeout = args.timeout
        self.keepalive_rate = args.keepalive_rate
        self.bearer = args.bearer
        self.client_ip_count = args.client_ip_count
        self.client_ip_prefix = args.client_ip_prefix
        now = int(time.time())
        _alg_none_hdr = b64url(json.dumps({"alg": "none", "typ": "JWT"}, separators=(",", ":")).encode())
        _alg_none_body = b64url(json.dumps(
            {"sub": "soak-algnone", "iss": args.jwt_issuer, "iat": now, "exp": now + 3600},
            separators=(",", ":")).encode())
        self.bad_tokens = (
            make_hs256_token(
                {"sub": "soak-expired", "iss": args.jwt_issuer, "iat": now - 7200, "exp": now - 3600},
                args.jwt_secret,
            ),
            make_hs256_token(
                {"sub": "soak-forged", "iss": args.jwt_issuer, "iat": now, "exp": now + 3600},
                "not-the-gateway-secret",
            ),
            # alg=none attack: unsigned token — gateway must reject with 401
            f"{_alg_none_hdr}.{_alg_none_body}.",
        )
        total = sum(args.weights.values())
        running = 0.0
        self.choice_table = []
        for name in DEFAULT_WEIGHTS:
            running += args.weights[name] / total
            self.choice_table.append((running, name))

    def scenario(self, rng):
        choice = rng.random()
        for ceiling, name in self.choice_table:
            if choice < ceiling:
                return name
        return self.choice_table[-1][1]

    def client_ip(self, worker_id):
        if not self.client_ip_count:
            return ""
        index = worker_id % self.client_ip_count
        return f"{self.client_ip_prefix}.{index // 254}.{index % 254 + 1}"

    def request_data(self, scenario, rng, worker_id):
        headers = {"Host": f"{self.host}:{self.port}"}
        client_ip = self.client_ip(worker_id)
        if client_ip:
            headers["X-Forwarded-For"] = client_ip
        method = "GET"
        path = "/api/users"
        body = None
        if scenario in ("normal", "post", "not_found", "disconnect"):
            headers["Authorization"] = "Bearer " + self.bearer
        elif scenario == "bad_token":
            headers["Authorization"] = "Bearer " + rng.choice(self.bad_tokens)
        if scenario == "post":
            method = "POST"
            size = rng.choice((0, 1024, 100 * 1024))
            body = rng.randbytes(size) if hasattr(rng, "randbytes") else bytes(rng.getrandbits(8) for _ in range(size))
            headers["Content-Type"] = "application/octet-stream"
        elif scenario == "not_found":
            path = "/api/other"
        return method, path, headers, body


def make_connection(workload):
    return http.client.HTTPConnection(workload.host, workload.port, timeout=workload.timeout)


def fast_disconnect(workload, method, path, headers, body):
    headers = dict(headers)
    headers["Connection"] = "close"
    if body is not None:
        headers["Content-Length"] = str(len(body))
    request_lines = [f"{method} {path} HTTP/1.1"]
    request_lines.extend(f"{key}: {value}" for key, value in headers.items())
    wire = ("\r\n".join(request_lines) + "\r\n\r\n").encode("iso-8859-1")
    if body:
        wire += body
    connection = socket.create_connection((workload.host, workload.port), timeout=workload.timeout)
    try:
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        connection.sendall(wire)
    finally:
        connection.close()


def worker(worker_id, deadline, stop, workload, stats, seed, pacer):
    rng = random.Random(seed + worker_id)
    keepalive_connection = None
    try:
        while not stop.is_set() and time.monotonic() < deadline:
            if pacer is not None and not pacer.wait(stop, deadline):
                break
            scenario = workload.scenario(rng)
            method, path, headers, body = workload.request_data(scenario, rng, worker_id)
            connection = None
            reuse = False
            try:
                if scenario == "disconnect":
                    fast_disconnect(workload, method, path, headers, body)
                    stats.disconnected(scenario)
                    continue

                reuse = rng.random() < workload.keepalive_rate
                if reuse:
                    if keepalive_connection is None:
                        keepalive_connection = make_connection(workload)
                    connection = keepalive_connection
                    headers["Connection"] = "keep-alive"
                else:
                    connection = make_connection(workload)
                    headers["Connection"] = "close"

                started = time.monotonic()
                connection.request(method, path, body=body, headers=headers)
                response = connection.getresponse()
                response.read()
                stats.response(scenario, response.status, (time.monotonic() - started) * 1000.0)

                if not reuse:
                    connection.close()
            except (OSError, socket.timeout, http.client.HTTPException) as error:
                stats.error(scenario, type(error).__name__)
                if connection is not None:
                    try:
                        connection.close()
                    except OSError:
                        pass
                if reuse:
                    keepalive_connection = None
    finally:
        if keepalive_connection is not None:
            try:
                keepalive_connection.close()
            except OSError:
                pass


def print_progress(stats):
    requests, _codes, errors, latencies = stats.snapshot()
    elapsed = max(0.001, time.monotonic() - stats.started)
    total = sum(requests.values())
    compact_counts = ",".join(f"{name}={requests.get(name, 0)}" for name in DEFAULT_WEIGHTS)
    print(
        f"progress elapsed={elapsed:.0f}s qps={total / elapsed:.1f} requests={total} "
        f"scenarios={compact_counts} errors={sum(errors.values())} "
        f"lat_ms_p50={percentile(latencies, .50):.1f} lat_ms_p99={percentile(latencies, .99):.1f}",
        flush=True,
    )


def print_summary(stats):
    requests, codes, errors, latencies = stats.snapshot()
    elapsed = max(0.001, time.monotonic() - stats.started)
    total = sum(requests.values())
    print("\n=== soak summary ===")
    print(f"duration_s={elapsed:.2f} attempts={total} qps={total / elapsed:.2f} transport_errors={sum(errors.values())}")
    if latencies:
        print(
            f"latency_ms count={len(latencies)} p50={percentile(latencies, .50):.2f} "
            f"p90={percentile(latencies, .90):.2f} p99={percentile(latencies, .99):.2f} max={latencies[-1]:.2f}"
        )
    print("scenario                                      attempts  codes")
    for name in DEFAULT_WEIGHTS:
        code_text = " ".join(f"{code}:{count}" for code, count in sorted(codes[name].items(), key=lambda item: str(item[0])))
        print(f"{name:11} {SCENARIO_LABELS[name]:29} {requests.get(name, 0):8}  {code_text or '-'}")
    print("transport_error_types=" + (" ".join(f"{name}:{count}" for name, count in sorted(errors.items())) or "none"))


def write_result(path, args, stats, seed, failures):
    if not path:
        return
    requests, codes, errors, latencies = stats.snapshot()
    result = {
        "seed": seed,
        "url": f"http://{args.host}:{args.port}",
        "workers": args.workers,
        "target_qps": args.target_qps,
        "seconds": args.seconds,
        "requests": requests,
        "codes": {name: {str(code): count for code, count in values.items()}
                  for name, values in codes.items()},
        "transport_errors": errors,
        "latency_ms": {
            "count": len(latencies),
            "p50": percentile(latencies, .50),
            "p90": percentile(latencies, .90),
            "p99": percentile(latencies, .99),
            "max": latencies[-1] if latencies else 0.0,
        },
        "failures": failures,
    }
    output = pathlib.Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + f".tmp.{os.getpid()}")
    temporary.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(output)


def evaluate(args, stats, worker_threads):
    requests, codes, errors, _latencies = stats.snapshot()
    failures = []
    for scenario, minimum in args.min_attempts:
        actual = requests.get(scenario, 0)
        if actual < minimum:
            failures.append(f"{scenario} attempts {actual} < {minimum}")
    for scenario, status, minimum in args.min_status_ratio:
        attempts = requests.get(scenario, 0)
        actual = codes[scenario].get(status, 0)
        ratio = actual / attempts if attempts else 0.0
        if ratio < minimum:
            failures.append(
                f"{scenario} HTTP {status} ratio {actual}/{attempts} ({ratio:.1%}) < {minimum:.1%}"
            )
    transport_errors = sum(errors.values())
    if args.max_transport_errors is not None and transport_errors > args.max_transport_errors:
        failures.append(f"transport errors {transport_errors} > {args.max_transport_errors}")
    alive = sum(thread.is_alive() for thread in worker_threads)
    if alive:
        failures.append(f"{alive} benchmark workers did not stop by deadline")
    return failures


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url", help="gateway base URL, for example http://127.0.0.1:18090")
    parser.add_argument("workers", nargs="?", type=int, default=20)
    parser.add_argument("seconds", nargs="?", type=float, default=1800.0)
    parser.add_argument("jwt_file", nargs="?", default="")
    parser.add_argument("--weights", type=parse_weights, default=DEFAULT_WEIGHTS.copy())
    parser.add_argument("--keepalive-rate", type=float, default=0.70)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--progress-seconds", type=float, default=10.0)
    parser.add_argument("--spike-every-seconds", type=float, default=5.0)
    parser.add_argument("--spike-probability", type=float, default=0.20)
    parser.add_argument("--spike-seconds", type=float, default=1.0)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--target-qps", type=float, default=0.0)
    parser.add_argument("--client-ip-count", type=int, default=0)
    parser.add_argument("--client-ip-prefix", default="198.18")
    parser.add_argument("--min-status-ratio", type=parse_status_ratio, action="append", default=[])
    parser.add_argument("--min-attempts", type=parse_min_attempts, action="append", default=[])
    parser.add_argument("--max-transport-errors", type=int)
    parser.add_argument("--result-json")
    parser.add_argument("--jwt-secret", default="change-me")
    parser.add_argument("--jwt-issuer", default="bronx")
    parser.add_argument("--jwt-sub", default="soak-client")
    parser.add_argument("--jwt-scope", default="")
    args = parser.parse_args()

    parsed = urlparse(args.url)
    if parsed.scheme != "http" or not parsed.hostname:
        parser.error("url must be an http:// URL with a host")
    args.host = parsed.hostname
    args.port = parsed.port or 80
    if parsed.path not in ("", "/") or parsed.params or parsed.query or parsed.fragment:
        parser.error("url must contain only scheme, host, and optional port")
    if args.workers < 1 or args.seconds <= 0 or args.timeout <= 0 or args.progress_seconds <= 0:
        parser.error("workers must be >= 1 and durations/timeouts must be positive")
    if args.target_qps < 0 or args.client_ip_count < 0:
        parser.error("target qps and client IP count cannot be negative")
    if args.max_transport_errors is not None and args.max_transport_errors < 0:
        parser.error("max transport errors cannot be negative")
    for name in ("keepalive_rate", "spike_probability"):
        if not 0.0 <= getattr(args, name) <= 1.0:
            parser.error(f"--{name.replace('_', '-')} must be between 0 and 1")
    if args.spike_every_seconds <= 0 or args.spike_seconds <= 0:
        parser.error("spike intervals must be positive")
    prefix = args.client_ip_prefix.split(".")
    if len(prefix) != 2 or any(not part.isdigit() or not 0 <= int(part) <= 255 for part in prefix):
        parser.error("client IP prefix must be two IPv4 octets, for example 198.18")
    if args.jwt_file:
        try:
            with open(args.jwt_file, encoding="utf-8") as token_file:
                args.bearer = token_file.read().strip()
        except OSError as error:
            parser.error(f"cannot read jwt_file: {error}")
        if not args.bearer:
            parser.error("jwt_file is empty")
    else:
        now = int(time.time())
        args.bearer = make_hs256_token(
            {"sub": args.jwt_sub, "iss": args.jwt_issuer, "iat": now,
             "exp": now + 24 * 60 * 60, "scope": args.jwt_scope},
            args.jwt_secret,
        )
    return args


def main():
    args = parse_args()
    workload = Workload(args)
    stats = Stats()
    stop = threading.Event()
    deadline = time.monotonic() + args.seconds
    seed = args.seed if args.seed is not None else random.SystemRandom().randrange(1 << 63)
    pacer = Pacer(args.target_qps) if args.target_qps else None

    base_threads = [
        threading.Thread(target=worker, args=(index, deadline, stop, workload, stats, seed, pacer))
        for index in range(args.workers)
    ]
    for thread in base_threads:
        thread.start()

    print(
        f"soak url=http://{args.host}:{args.port} workers={args.workers} seconds={args.seconds:.0f} "
        f"target_qps={args.target_qps:.1f} seed={seed} keepalive_rate={args.keepalive_rate:.0%} "
        f"client_ips={args.client_ip_count} weights={args.weights}",
        flush=True,
    )
    next_progress = stats.started + args.progress_seconds
    next_spike = stats.started + args.spike_every_seconds
    spike_id = args.workers
    scheduler_rng = random.Random(seed ^ 0x5A17)
    spike_threads = []
    try:
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_progress:
                print_progress(stats)
                next_progress += args.progress_seconds
            if now >= next_spike:
                next_spike += args.spike_every_seconds
                if scheduler_rng.random() < args.spike_probability:
                    spike_deadline = min(deadline, now + args.spike_seconds)
                    print(f"spike workers=+{args.workers} seconds={spike_deadline - now:.1f}", flush=True)
                    new_spikes = [
                        threading.Thread(
                            target=worker,
                            args=(spike_id + index, spike_deadline, stop, workload, stats, seed, pacer),
                        )
                        for index in range(args.workers)
                    ]
                    spike_id += args.workers
                    spike_threads.extend(new_spikes)
                    for thread in new_spikes:
                        thread.start()
            time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
    except KeyboardInterrupt:
        print("interrupted, stopping workers", flush=True)
    finally:
        stop.set()
        all_threads = base_threads + spike_threads
        for thread in all_threads:
            thread.join(timeout=args.timeout + 1.0)
        print_progress(stats)
        print_summary(stats)

    failures = evaluate(args, stats, all_threads)
    write_result(args.result_json, args, stats, seed, failures)
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        sys.exit(1)
    print("PASS: benchmark checks ok")


if __name__ == "__main__":
    main()
