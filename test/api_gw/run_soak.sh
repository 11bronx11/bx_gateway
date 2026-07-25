#!/usr/bin/env bash
# Real-process gateway soak. It never uses the checkout's runtime YAML, ports, logs, or database.
set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$script_dir/../.." && pwd -P)"
gw_source="${API_GW_SOAK_GW_BIN:-$repo/bin-address-undefined/gw}"
hub_source="${API_GW_SOAK_HUB_BIN:-$repo/bin-address-undefined/hub}"
banctl_source="${API_GW_SOAK_BANCTL_BIN:-$repo/bin/banctl}"
mock_py="$script_dir/mock_rich.py"
bench_py="$script_dir/bench_rich.py"
probes_py="$script_dir/gateway_probes.py"

baseline_seconds="${SOAK_SECONDS:-1800}"
probe_only="${SOAK_PROBE_ONLY:-0}"
fault_seconds="${SOAK_FAULT_SECONDS:-90}"
rate_seconds="${SOAK_RATE_SECONDS:-30}"
recovery_seconds="${SOAK_RECOVERY_SECONDS:-30}"
workers="${SOAK_WORKERS:-20}"
base_qps="${SOAK_QPS:-80}"
burst_qps="${SOAK_BURST_QPS:-160}"
multi_upstream_seconds="${SOAK_MULTI_UPSTREAM_SECONDS:-30}"
multi_upstream_qps="${SOAK_MULTI_UPSTREAM_QPS:-90}"
multi_upstream_min_requests="${SOAK_MULTI_UPSTREAM_MIN_REQUESTS:-20}"
fault_qps="${SOAK_FAULT_QPS:-40}"
rate_qps="${SOAK_RATE_QPS:-40}"
client_ips="${SOAK_CLIENT_IPS:-64}"
seed="${SOAK_SEED:-20260721}"
reload_seconds="${SOAK_RELOAD_SECONDS:-5}"
log_reload_seconds="${SOAK_LOG_RELOAD_SECONDS:-10}"
monitor_seconds="${SOAK_MONITOR_SECONDS:-5}"
term_timeout="${SOAK_TERM_TIMEOUT_SECONDS:-8}"
min_scenario_attempts="${SOAK_MIN_SCENARIO_ATTEMPTS:-20}"
max_fd_growth="${SOAK_MAX_FD_GROWTH:-32}"
max_rss_growth_kb="${SOAK_MAX_RSS_GROWTH_KB:-65536}"
max_hub_fd_growth="${SOAK_MAX_HUB_FD_GROWTH:-16}"
max_hub_rss_growth_kb="${SOAK_MAX_HUB_RSS_GROWTH_KB:-32768}"
jwt_secret="${SOAK_JWT_SECRET:-change-me}"
jwt_issuer="${SOAK_JWT_ISSUER:-bronx}"
jwt_scope="${SOAK_JWT_SCOPE:-read write}"
api_key="${SOAK_API_KEY:-soak-write-key}"
limited_api_key="${SOAK_LIMITED_API_KEY:-soak-read-key}"
k6_duration="${SOAK_K6_DURATION:-}"
k6_bin="${K6:-k6}"
k6_normal_rps="${SOAK_K6_NORMAL_RPS:-20}"

run_dir=""
runtime_root=""
business_port=""
admin_port=""
upstream_a_port=""
upstream_b_port=""
upstream_c_port=""
hub_http_port=""
submit_sock=""
subscribe_sock=""
hub_admin_sock=""
gw_pid=""
hub_pid=""
mock_a_pid=""
mock_b_pid=""
mock_c_pid=""
reload_pid=""
log_reload_pid=""
monitor_pid=""
keepalive_pid=""
slow_pid=""
failures=0
cleanup_forced=0
is_asan_binary=0

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    failures=$((failures + 1))
}

die() {
    printf 'run_soak: %s\n' "$*" >&2
    exit 2
}

pid_running() {
    local pid="${1:-}"
    local state
    [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null || return 1
    state="$(ps -o stat= -p "$pid" 2>/dev/null | tr -d '[:space:]')"
    [[ -n "$state" && "$state" != Z* ]]
}

stop_pid() {
    local label="$1"
    local pid="${2:-}"
    local timeout="$3"
    local end
    [[ -n "$pid" ]] || return 0
    if ! pid_running "$pid"; then
        wait "$pid" 2>/dev/null || true
        return 0
    fi
    kill -TERM "$pid" 2>/dev/null || true
    end=$((SECONDS + timeout))
    while pid_running "$pid" && (( SECONDS < end )); do
        sleep 0.1
    done
    if pid_running "$pid"; then
        printf 'run_soak: %s pid=%s ignored TERM for %ss, sending KILL\n' "$label" "$pid" "$timeout" >&2
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        return 1
    fi
    wait "$pid" 2>/dev/null || true
    return 0
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    stop_pid "reload loop" "$reload_pid" 3 || true
    stop_pid "framework log reload loop" "$log_reload_pid" 3 || true
    stop_pid "resource monitor" "$monitor_pid" 3 || true
    stop_pid "keep-alive probe" "$keepalive_pid" 3 || true
    stop_pid "slow request" "$slow_pid" 3 || true
    if ! stop_pid "gateway" "$gw_pid" "$term_timeout"; then
        cleanup_forced=1
    fi
    if ! stop_pid "hub" "$hub_pid" "$term_timeout"; then
        cleanup_forced=1
    fi
    stop_pid "mock upstream A" "$mock_a_pid" 3 || true
    stop_pid "mock upstream B" "$mock_b_pid" 3 || true
    stop_pid "mock upstream C" "$mock_c_pid" 3 || true
    if (( cleanup_forced && status == 0 )); then
        status=1
    fi
    if [[ -n "$run_dir" ]]; then
        printf 'run_soak: artifacts retained in %s\n' "$run_dir"
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT TERM

require_positive() {
    python3 - "$@" <<'PY'
import sys
for raw in sys.argv[1:]:
    try:
        if float(raw) <= 0:
            raise ValueError
    except ValueError:
        raise SystemExit(f"expected positive number, got {raw!r}")
PY
}

alloc_ports() {
    # 在同一个 Python 进程内一次性绑定所有端口，避免 bind+close 后 OS 重复分配的竞态
    python3 - "$1" <<'PY'
import socket, sys
count = int(sys.argv[1])
socks = []
for _ in range(count):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    socks.append(s)
for s in socks:
    print(s.getsockname()[1])
for s in socks:
    s.close()
PY
}

http_request() {
    local method="$1"
    local url="$2"
    local body="$3"
    local output="$4"
    python3 - "$method" "$url" "$body" "$output" <<'PY'
import http.client
import os
import pathlib
import sys
from urllib.parse import urlsplit

method, raw_url, body, output = sys.argv[1:]
url = urlsplit(raw_url)
if url.scheme != "http" or not url.hostname:
    raise SystemExit("invalid HTTP URL: " + raw_url)
path = url.path or "/"
if url.query:
    path += "?" + url.query
headers = {}
payload = body.encode("utf-8") if body else None
if payload is not None:
    headers["Content-Type"] = "application/json"
connection = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=3)
try:
    connection.request(method, path, body=payload, headers=headers)
    response = connection.getresponse()
    data = response.read()
    status = response.status
finally:
    connection.close()
if output != "-":
    path = pathlib.Path(output)
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    temporary.write_bytes(data)
    temporary.replace(path)
print(status)
PY
}

wait_http() {
    local url="$1"
    local pid="$2"
    local attempts="${3:-100}"
    local code
    for ((i = 0; i < attempts; ++i)); do
        if ! pid_running "$pid"; then
            return 1
        fi
        code="$(http_request GET "$url" "" - 2>/dev/null || true)"
        if [[ "$code" == 2* ]]; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

write_gateway() {
    local capacity="$1"
    local refill="$2"
    local total_timeout="$3"
    local read_timeout="$4"
    local rate_report_enabled="${5:-false}"
    local rate_report_hits="${6:-10}"
    local rate_report_window_ms="${7:-60000}"
    local rate_report_ban_ms="${8:-30000}"
    python3 - "$runtime_root/api_gw/bin/gateway.yml" "$business_port" "$admin_port" \
        "$upstream_a_port" "$upstream_b_port" "$upstream_c_port" \
        "$capacity" "$refill" "$total_timeout" "$read_timeout" "$jwt_secret" "$jwt_issuer" \
        "$api_key" "$limited_api_key" "$rate_report_enabled" "$rate_report_hits" \
        "$rate_report_window_ms" "$rate_report_ban_ms" <<'PY'
import os
import pathlib
import json
import hashlib
import sys

path, business, admin, upstream_a, upstream_b, upstream_c, capacity, refill, total_timeout, read_timeout, secret, issuer, api_key, limited_api_key, rate_enabled, rate_hits, rate_window, rate_ban = sys.argv[1:]
secret = json.dumps(secret)
issuer = json.dumps(issuer)
api_key_hash = hashlib.sha256(api_key.encode()).hexdigest()
limited_api_key_hash = hashlib.sha256(limited_api_key.encode()).hexdigest()
text = f'''server:
  address: "127.0.0.1:{business}"
  admin_address: "127.0.0.1:{admin}"
  io_workers: 4
  maintenance: false
trusted_proxies:
  - 127.0.0.1/32
upstreams:
  - name: api
    lb: round_robin
    timeout:
      total_ms: {total_timeout}
      connect_ms: 500
      read_ms: {read_timeout}
    circuit_breaker:
      failure_threshold: 5
      window_ms: 10000
      min_requests: 10
      failure_rate: 50
      slow_ms: 1000
      slow_rate: 50
      open_timeout_ms: 2000
      max_open_timeout_ms: 8000
      half_open_max_requests: 1
      half_open_successes: 1
      failure_statuses: [500, 502, 503, 504]
    connection_pool:
      max_idle: 32
      idle_timeout_ms: 5000
    health_check:
      enabled: true
      path: /_soak/health
      interval_ms: 100
      timeout_ms: 300
      healthy_threshold: 1
      unhealthy_threshold: 1
    endpoints:
      - {{ host: "127.0.0.1", port: {upstream_a}, weight: 1 }}
      - {{ host: "127.0.0.1", port: {upstream_b}, weight: 1 }}
      - {{ host: "127.0.0.1", port: {upstream_c}, weight: 1 }}
routes:
  - name: api
    path: /api
    match_type: prefix
    upstream: api
    strip_prefix: true
    auth: jwt
    require_scopes: [read]
    rate_limit:
      enabled: true
      capacity: {capacity}
      refill_per_sec: {refill}
      key: ip
  - name: public
    path: /public
    match_type: exact
    upstream: api
    strip_prefix: true
    auth: none
  - name: apikey
    path: /apikey
    match_type: exact
    upstream: api
    strip_prefix: true
    auth: api_key
    require_scopes: [write]
  - name: ws
    path: /ws
    match_type: prefix
    upstream: api
    strip_prefix: true
    auth: jwt
    require_scopes: [read]
auth:
  jwt:
    enabled: true
    algo: HS256
    secret: {secret}
    issuer: {issuer}
  api_key:
    header: "X-API-Key"
    keys:
      - id: soak-write
        hash: "{api_key_hash}"
        enabled: true
        scopes: [read, write]
      - id: soak-read
        hash: "{limited_api_key_hash}"
        enabled: true
        scopes: [read]
  forward:
    enabled: true
    strip_bearer: true
    strip_api_key: true
cors:
  enabled: true
  allow_origins:
    - https://client.example
  allow_methods: GET, POST, OPTIONS
  allow_headers: X-Test
ip_filter:
  enabled: false
  mode: denylist
  cidrs: []
ip_policy:
  submit_sock: "{pathlib.Path(path).parent.parent.parent / 'submit.sock'}"
  subscribe_sock: "{pathlib.Path(path).parent.parent.parent / 'subscribe.sock'}"
  instance_id: "soak"
  waf:
    enabled: true
    ban_ms: 60000
  rate_report:
    enabled: {rate_enabled}
    hits: {rate_hits}
    window_ms: {rate_window}
    ban_ms: {rate_ban}
'''
output = pathlib.Path(path)
temporary = output.with_name(output.name + f".tmp.{os.getpid()}")
temporary.write_text(text, encoding="utf-8")
temporary.replace(output)
PY
}

write_hub() {
    python3 - "$runtime_root/api_gw/bin/hub.yml" "$submit_sock" "$subscribe_sock" \
        "$hub_admin_sock" "$hub_http_port" "$runtime_root/api_gw/bin/hub.db" <<'PY'
import os
import pathlib
import sys

path, submit, subscribe, admin, http_port, db = sys.argv[1:]
text = f'''server:
  submit_sock: "{submit}"
  subscribe_sock: "{subscribe}"
  admin_sock: "{admin}"
  http_address: "127.0.0.1:{http_port}"
  db_path: "{db}"
  iom_workers: 2
'''
output = pathlib.Path(path)
temporary = output.with_name(output.name + f".tmp.{os.getpid()}")
temporary.write_text(text, encoding="utf-8")
temporary.replace(output)
PY
}

write_framework_log_config() {
    local mode="$1"
    local sample_rate="$2"
    python3 - "$runtime_root/api_gw/bin/bronx.yml" "$mode" "$sample_rate" <<'PY'
import os
import pathlib
import re
import sys

path, mode, sample_rate = sys.argv[1:]
if not re.fullmatch(r"[a-z0-9-]+", mode):
    raise SystemExit(f"invalid log mode: {mode!r}")
if not sample_rate.isdigit():
    raise SystemExit(f"invalid log sample rate: {sample_rate!r}")

text = f'''cpu_pool:
  threads: 2
  max_queue: 0
  name: soak-cpu
fiber:
  stack_size: 131072
  stack_pool_size: 8
  guard_page: 1
tcp:
  connect:
    timeout: 5000
tcp_server:
  recv_timeout: 120000
logs:
  - name: root
    level: info
    appenders:
      - type: BxFileLogAppender
        file: logs/framework-{mode}.log
        pattern: "[SOAK-{mode}-ROOT][%p] %m%n"
        async: true
  - name: gw
    level: info
    sample_rate: {sample_rate}
    appenders:
      - type: BxFileLogAppender
        file: logs/gw-{mode}.log
        pattern: "[SOAK-{mode}-GW][%p] %m%n"
        async: true
'''
output = pathlib.Path(path)
temporary = output.with_name(output.name + f".tmp.{os.getpid()}")
temporary.write_text(text, encoding="utf-8")
temporary.replace(output)
PY
}

write_hub_framework_config() {
    python3 - "$runtime_root/api_gw/bin/hub_bronx.yml" <<'PY'
import os
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
text = '''cpu_pool:
  threads: 2
  max_queue: 0
  name: soak-hub-cpu
fiber:
  stack_size: 131072
  stack_pool_size: 8
  guard_page: 1
tcp:
  connect:
    timeout: 5000
tcp_server:
  recv_timeout: 120000
logs:
  - name: root
    level: info
    appenders:
      - type: BxFileLogAppender
        file: logs/hub-framework.log
        pattern: "[SOAK-HUB-ROOT][%p] %m%n"
        async: true
  - name: hub
    level: info
    appenders:
      - type: BxFileLogAppender
        file: logs/hub.log
        pattern: "[SOAK-HUB][%p] %m%n"
        async: true
'''
temporary = output.with_name(output.name + f".tmp.{os.getpid()}")
temporary.write_text(text, encoding="utf-8")
temporary.replace(output)
PY
}

admin_reload() {
    local code
    code="$(http_request POST "http://127.0.0.1:$admin_port/reload" "" "$run_dir/reload-body.txt" || true)"
    [[ "$code" == "200" ]] && assert_admin_routes
}

assert_admin_routes() {
    [[ "$(http_request GET "http://127.0.0.1:$admin_port/routes" "" "$run_dir/routes.json" || true)" == "200" ]] || return 1
    python3 - "$run_dir/routes.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
routes = {route.get("name"): route for route in data.get("routes", [])}
for name in ("api", "public", "apikey", "ws"):
    if name not in routes:
        raise SystemExit("missing route " + name)
api = routes["api"]
if (api.get("path"), api.get("upstream"), api.get("auth")) != ("/api", "api", "jwt"):
    raise SystemExit("api route does not match live config: " + repr(api))
if not api.get("rate_limit", {}).get("enabled"):
    raise SystemExit("api route lost rate limit")
PY
}

mock_control() {
    local payload="$1"
    mock_control_at "$upstream_a_port" "$payload"
}

mock_control_at() {
    local port="$1"
    local payload="$2"
    [[ "$(http_request POST "http://127.0.0.1:$port/_soak/control" "$payload" \
        "$run_dir/mock-control-$port.json" || true)" == "200" ]]
}

reset_mock_stats() {
    local payload='{"error_probability":0,"drop_probability":0,"slow_probability":0,"healthy":true,"reset_stats":true}'
    mock_control_at "$upstream_a_port" "$payload" \
        && mock_control_at "$upstream_b_port" "$payload" \
        && mock_control_at "$upstream_c_port" "$payload"
}

assert_api_upstreams_healthy() {
    local end=$((SECONDS + 12))
    while (( SECONDS < end )); do
        if snapshot_routes "api-health" && python3 - "$run_dir/routes-api-health.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
group = next((item for item in data.get("upstreams", []) if item.get("name") == "api"), None)
endpoints = group.get("endpoints", []) if group else []
if len(endpoints) != 3:
    raise SystemExit("api endpoint count=" + str(len(endpoints)))
if not all(item.get("healthy") and item.get("health_checks", 0) > 0 for item in endpoints):
    raise SystemExit("api endpoints have not all passed health checks: " + repr(endpoints))
PY
        then
            return 0
        fi
        if ! pid_running "$gw_pid"; then
            return 1
        fi
        sleep 0.1
    done
    return 1
}

snapshot_mock_stats() {
    local name="$1"
    local port="$2"
    [[ "$(http_request GET "http://127.0.0.1:$port/_soak/stats" "" \
        "$run_dir/mock-$name-stats.json" || true)" == "200" ]]
}

assert_multi_upstream_load() {
    snapshot_mock_stats "a" "$upstream_a_port" \
        && snapshot_mock_stats "b" "$upstream_b_port" \
        && snapshot_mock_stats "c" "$upstream_c_port" || return 1
    python3 - "$run_dir/mock-a-stats.json" "$run_dir/mock-b-stats.json" \
        "$run_dir/mock-c-stats.json" "$multi_upstream_min_requests" <<'PY'
import json
import math
import sys

stats = [json.load(open(path, encoding="utf-8")) for path in sys.argv[1:4]]
minimum = int(sys.argv[4])
names = [item.get("name") for item in stats]
counts = [int(item.get("requests", 0)) for item in stats]
if names != ["soak-a", "soak-b", "soak-c"]:
    raise SystemExit("unexpected mock identities: " + repr(names))
total = sum(counts)
floor = max(minimum, math.ceil(total * 0.20))
ceiling = math.floor(total * 0.45)
if total == 0 or any(count < floor or count > ceiling for count in counts):
    raise SystemExit("round-robin load is not spread across all upstreams: "
                     + repr(dict(zip(names, counts)))
                     + f" total={total} required=[{floor},{ceiling}]")
PY
}

assert_circuit_fault_isolated() {
    snapshot_mock_stats "circuit-a" "$upstream_a_port" \
        && snapshot_mock_stats "circuit-b" "$upstream_b_port" \
        && snapshot_mock_stats "circuit-c" "$upstream_c_port" || return 1
    python3 - "$run_dir/mock-circuit-a-stats.json" "$run_dir/mock-circuit-b-stats.json" \
        "$run_dir/mock-circuit-c-stats.json" <<'PY'
import json
import sys

stats = [json.load(open(path, encoding="utf-8")) for path in sys.argv[1:4]]
by_name = {item.get("name"): item for item in stats}
if set(by_name) != {"soak-a", "soak-b", "soak-c"}:
    raise SystemExit("unexpected mock identities: " + repr(sorted(by_name)))
if int(by_name["soak-a"].get("responses", {}).get("503", 0)) < 5:
    raise SystemExit("faulting upstream did not emit the expected 503 burst: " + repr(by_name["soak-a"]))
for name in ("soak-b", "soak-c"):
    if int(by_name[name].get("responses", {}).get("200", 0)) < 5:
        raise SystemExit("healthy upstream did not carry successful fault-phase traffic: "
                         + name + "=" + repr(by_name[name]))
PY
}

snapshot_stats() {
    local name="$1"
    [[ "$(http_request GET "http://127.0.0.1:$admin_port/stats" "" \
        "$run_dir/stats-$name.json" || true)" == "200" ]] || return 1
    python3 -m json.tool "$run_dir/stats-$name.json" >/dev/null
}

snapshot_routes() {
    local name="$1"
    [[ "$(http_request GET "http://127.0.0.1:$admin_port/routes" "" \
        "$run_dir/routes-$name.json" || true)" == "200" ]] || return 1
    python3 -m json.tool "$run_dir/routes-$name.json" >/dev/null
}

json_at_least() {
    local file="$1"
    local key="$2"
    local minimum="$3"
    python3 - "$file" "$key" "$minimum" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
value = data
for part in sys.argv[2].split('.'):
    value = value[part]
if float(value) < float(sys.argv[3]):
    raise SystemExit(f"{sys.argv[2]}={value} < {sys.argv[3]}")
PY
}

json_delta_at_least() {
    local before="$1"
    local after="$2"
    local key="$3"
    local minimum="$4"
    python3 - "$before" "$after" "$key" "$minimum" <<'PY'
import json
import sys

def get(data, key):
    value = data
    for part in key.split('.'):
        value = value[part]
    return float(value)

before = json.load(open(sys.argv[1], encoding="utf-8"))
after = json.load(open(sys.argv[2], encoding="utf-8"))
delta = get(after, sys.argv[3]) - get(before, sys.argv[3])
if delta < float(sys.argv[4]):
    raise SystemExit(f"{sys.argv[3]} delta={delta} < {sys.argv[4]}")
PY
}

assert_active_zero() {
    local name="$1"
    snapshot_stats "$name"
    local active
    if ! active="$(python3 - "$run_dir/stats-$name.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
if "active_conns" not in data:
    raise SystemExit("active_conns key missing from stats")
print(data["active_conns"])
PY
    )"; then
        fail "cannot read active_conns after $name"
        return
    fi
    [[ "$active" == "0" ]] || fail "active_conns remains $active after $name"
}

assert_circuit_opened() {
    [[ "$(http_request GET "http://127.0.0.1:$admin_port/metrics" "" \
        "$run_dir/metrics-circuit.prom" || true)" == "200" ]] || return 1
    python3 - "$run_dir/metrics-circuit.prom" <<'PY'
import re
import sys

for line in open(sys.argv[1], encoding="utf-8"):
    if 'gateway_circuit_transitions_total{' in line and 'from="closed",to="open"' in line:
        if float(line.rsplit(" ", 1)[1]) > 0:
            raise SystemExit(0)
raise SystemExit("no closed-to-open circuit transition")
PY
}

assert_circuit_closed() {
    snapshot_stats "circuit-recovered"
    python3 - "$run_dir/stats-circuit-recovered.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
states = [upstream.get("circuit", {}).get("state")
          for upstream in data.get("upstreams", [])]
if not states or any(state != "closed" for state in states):
    raise SystemExit("circuit not closed: " + repr(states))
PY
}

start_resource_monitor() {
    (
        printf 'epoch_s,gw_fd,gw_rss_kb,gw_threads,hub_fd,hub_rss_kb,hub_threads,active_conns\n'
        while pid_running "$gw_pid"; do
            gw_fd="$(find "/proc/$gw_pid/fd" -mindepth 1 -maxdepth 1 -printf . 2>/dev/null | wc -c)"
            gw_rss="$(awk '/VmRSS:/ {print $2}' "/proc/$gw_pid/status" 2>/dev/null || printf 0)"
            gw_threads="$(awk '/Threads:/ {print $2}' "/proc/$gw_pid/status" 2>/dev/null || printf 0)"
            hub_fd="$(find "/proc/$hub_pid/fd" -mindepth 1 -maxdepth 1 -printf . 2>/dev/null | wc -c)"
            hub_rss="$(awk '/VmRSS:/ {print $2}' "/proc/$hub_pid/status" 2>/dev/null || printf 0)"
            hub_threads="$(awk '/Threads:/ {print $2}' "/proc/$hub_pid/status" 2>/dev/null || printf 0)"
            if [[ "$(http_request GET "http://127.0.0.1:$admin_port/stats" "" \
                "$run_dir/monitor-stats.json" 2>/dev/null || true)" == "200" ]]; then
                active="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("active_conns", -1))' \
                    "$run_dir/monitor-stats.json" 2>/dev/null || printf '%s' -1)"
            else
                active=-1
            fi
            printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$(date +%s)" "$gw_fd" "$gw_rss" "$gw_threads" \
                "$hub_fd" "$hub_rss" "$hub_threads" "$active"
            sleep "$monitor_seconds"
        done
    ) >"$run_dir/resources.csv" 2>&1 &
    monitor_pid=$!
}

assert_resource_budget() {
    python3 - "$run_dir/resources.csv" "$max_fd_growth" "$max_rss_growth_kb" \
        "$max_hub_fd_growth" "$max_hub_rss_growth_kb" <<'PY'
import csv
import sys

rows = list(csv.DictReader(open(sys.argv[1], encoding="utf-8")))
fields = ("gw_fd", "gw_rss_kb", "gw_threads", "hub_fd", "hub_rss_kb", "hub_threads")
rows = [row for row in rows if all(row.get(field, "").isdigit() for field in fields)]
if len(rows) < 2:
    raise SystemExit("insufficient resource samples")
first, last = rows[0], rows[-1]
limits = {
    "gw": (int(sys.argv[2]), int(sys.argv[3])),
    "hub": (int(sys.argv[4]), int(sys.argv[5])),
}
for name, (fd_limit, rss_limit) in limits.items():
    fd_growth = int(last[f"{name}_fd"]) - int(first[f"{name}_fd"])
    rss_growth = int(last[f"{name}_rss_kb"]) - int(first[f"{name}_rss_kb"])
    if fd_growth > fd_limit:
        raise SystemExit(f"{name} fd growth {fd_growth} exceeds {fd_limit}")
    if rss_growth > rss_limit:
        raise SystemExit(f"{name} rss growth {rss_growth}KB exceeds {rss_limit}KB")
    first_threads = first[name + "_threads"]
    last_threads = last[name + "_threads"]
    if int(last_threads) != int(first_threads):
        raise SystemExit(f"{name} thread count changed {first_threads} -> {last_threads}")
PY
}

wait_hub_ready() {
    local code
    for ((i = 0; i < 100; ++i)); do
        if ! pid_running "$hub_pid"; then
            return 1
        fi
        if [[ -S "$submit_sock" && -S "$subscribe_sock" && -S "$hub_admin_sock" ]]; then
            code="$(http_request GET "http://127.0.0.1:$hub_http_port/healthz" "" - 2>/dev/null || true)"
            if [[ "$code" == 2* ]]; then
                return 0
            fi
        fi
        sleep 0.1
    done
    return 1
}

start_hub() {
    (
        cd "$runtime_root"
        ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:use_sigaltstack=0:log_path=$run_dir/asan-hub" \
        UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=0' exec "$runtime_root/bin/hub"
    ) >>"$run_dir/hub.log" 2>&1 &
    hub_pid=$!
    wait_hub_ready
}

restart_hub() {
    if ! stop_pid "hub restart" "$hub_pid" "$term_timeout"; then
        return 1
    fi
    hub_pid=""
    if [[ -e "$submit_sock" || -e "$subscribe_sock" || -e "$hub_admin_sock" ]]; then
        return 1
    fi
    start_hub
}

run_ipban_probe() {
    local mode="$1"
    local output="$2"
    python3 "$probes_py" \
        --gateway-url "http://127.0.0.1:$business_port" \
        --admin-url "http://127.0.0.1:$admin_port" \
        --mock-url "http://127.0.0.1:$upstream_a_port" \
        --mock-url "http://127.0.0.1:$upstream_b_port" \
        --mock-url "http://127.0.0.1:$upstream_c_port" \
        --hub-url "http://127.0.0.1:$hub_http_port" \
        --banctl "$runtime_root/bin/banctl" --banctl-sock "$hub_admin_sock" \
        --jwt-secret "$jwt_secret" --jwt-issuer "$jwt_issuer" \
        --api-key "$api_key" --limited-api-key "$limited_api_key" \
        --mode "$mode" --rate-report-hits 3 --result-json "$run_dir/$output"
}

materialize_app() {
    local source="$1"
    local name="$2"
    if ! ln "$source" "$runtime_root/bin/$name" 2>/dev/null; then
        cp "$source" "$runtime_root/bin/$name"
    fi
}

log_count() {
    local path="$1"
    local marker="$2"
    python3 - "$path" "$marker" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
marker = sys.argv[2]
if not path.exists():
    print(0)
else:
    print(path.read_text(encoding="utf-8", errors="replace").count(marker))
PY
}

wait_log_count() {
    local path="$1"
    local marker="$2"
    local expected="$3"
    local timeout="$4"
    local end=$((SECONDS + timeout))
    local actual
    while (( SECONDS < end )); do
        actual="$(log_count "$path" "$marker")"
        if (( actual >= expected )); then
            return 0
        fi
        if ! pid_running "$gw_pid"; then
            return 1
        fi
        sleep 0.1
    done
    actual="$(log_count "$path" "$marker")"
    printf 'run_soak: log marker=%q expected>=%s actual=%s file=%s\n' \
        "$marker" "$expected" "$actual" "$path" >&2
    return 1
}

signal_hup_expect_gw_log() {
    local mode="$1"
    local expected="$2"
    local log_path="$runtime_root/logs/gw-$mode.log"
    kill -HUP "$gw_pid" 2>/dev/null || return 1
    wait_log_count "$log_path" "framework config reloaded" "$expected" 12 \
        && wait_http "http://127.0.0.1:$admin_port/healthz" "$gw_pid" 20
}

write_logrotate_config() {
    local log_path="$1"
    python3 - "$run_dir/logrotate.conf" "$log_path" <<'PY'
import os
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
text = f'''{sys.argv[2]} {{
    size 1k
    rotate 2
    missingok
    notifempty
    nocompress
    create 0644
}}
'''
temporary = output.with_name(output.name + f".tmp.{os.getpid()}")
temporary.write_text(text, encoding="utf-8")
temporary.replace(output)
PY
}

exercise_log_sample_limit() {
    local root_log="$runtime_root/logs/framework-limit.log"
    local gw_log="$runtime_root/logs/gw-limit.log"
    local sent=8
    local root_before
    local gw_before
    local gw_after
    local started_ns
    local elapsed_ms
    local max_emitted

    write_framework_log_config "limit" 1
    signal_hup_expect_gw_log "limit" 1 || {
        fail "SIGHUP did not apply log sample-rate configuration"
        return
    }
    root_before="$(log_count "$root_log" "received signal 1, reloading")"
    gw_before="$(log_count "$gw_log" "framework config reloaded")"
    started_ns="$(date +%s%N)"
    for ((i = 0; i < sent; ++i)); do
        kill -HUP "$gw_pid" 2>/dev/null || {
            fail "cannot send SIGHUP during log sample-limit test"
            return
        }
        sleep 0.35
    done
    if ! wait_log_count "$root_log" "received signal 1, reloading" "$((root_before + sent))" 12; then
        fail "SIGHUP delivery was incomplete during log sample-limit test"
        return
    fi
    sleep 3.2
    elapsed_ms=$(( ($(date +%s%N) - started_ns) / 1000000 ))
    gw_after="$(log_count "$gw_log" "framework config reloaded")"
    max_emitted=$((1 + (elapsed_ms + 999) / 1000))
    if (( gw_after - gw_before > max_emitted )); then
        fail "log sample_rate=1 emitted $((gw_after - gw_before)) lines in ${elapsed_ms}ms, limit=$max_emitted"
    fi
    if (( gw_after - gw_before >= sent )); then
        fail "log sample_rate=1 did not shed any of $sent delivered SIGHUP reload logs"
    fi
}

exercise_log_rotation() {
    local root_log="$runtime_root/logs/framework-rotate.log"
    local gw_log="$runtime_root/logs/gw-rotate.log"
    local sent=28
    local root_before
    local gw_before
    local size

    write_framework_log_config "rotate" 0
    signal_hup_expect_gw_log "rotate" 1 || {
        fail "SIGHUP did not apply log rotation configuration"
        return
    }
    root_before="$(log_count "$root_log" "received signal 1, reloading")"
    for ((i = 0; i < sent; ++i)); do
        kill -HUP "$gw_pid" 2>/dev/null || {
            fail "cannot send SIGHUP while filling log for rotation"
            return
        }
        sleep 0.25
    done
    if ! wait_log_count "$root_log" "received signal 1, reloading" "$((root_before + sent))" 12; then
        fail "SIGHUP delivery was incomplete while filling log for rotation"
        return
    fi
    sleep 3.2
    size="$(stat -c %s "$root_log" 2>/dev/null || printf 0)"
    if (( size < 1024 )); then
        fail "log rotation input stayed below 1KiB, got ${size} bytes"
        return
    fi
    write_logrotate_config "$root_log"
    if ! logrotate -s "$run_dir/logrotate.state" "$run_dir/logrotate.conf" >"$run_dir/logrotate.log" 2>&1; then
        fail "logrotate failed for isolated gateway log"
        return
    fi
    if [[ ! -s "$root_log.1" || ! -f "$root_log" ]]; then
        fail "logrotate did not retain archive and create current log"
        return
    fi
    gw_before="$(log_count "$gw_log" "framework config reloaded")"
    root_before="$(log_count "$root_log" "received signal 1, reloading")"
    sleep 3.2
    if ! signal_hup_expect_gw_log "rotate" "$((gw_before + 1))"; then
        fail "gateway did not reopen the current log after rotation"
        return
    fi
    if ! wait_log_count "$root_log" "received signal 1, reloading" "$((root_before + 1))" 12; then
        fail "gateway did not reopen the root log after rotation"
        return
    fi
    if [[ ! -s "$root_log" ]] || ! grep -Fq "received signal 1, reloading" "$root_log"; then
        fail "post-rotation reload marker was not written to the current log"
    fi
}

run_bench() {
    local name="$1"
    local seconds="$2"
    local phase_workers="$3"
    local qps="$4"
    local ips="$5"
    local weights="$6"
    shift 6
    local phase_seed=$((seed + ${#name} * 7919))
    printf '\n=== phase %s seconds=%s workers=%s qps=%s ===\n' "$name" "$seconds" "$phase_workers" "$qps"
    if ! python3 "$bench_py" "http://127.0.0.1:$business_port" "$phase_workers" "$seconds" \
        --seed "$phase_seed" --target-qps "$qps" --client-ip-count "$ips" --weights "$weights" \
        --jwt-secret "$jwt_secret" --jwt-issuer "$jwt_issuer" --jwt-scope "$jwt_scope" --progress-seconds 10 \
        --result-json "$run_dir/bench-$name.json" "$@" 2>&1 | tee "$run_dir/bench-$name.log"; then
        fail "benchmark phase $name"
    fi
}

gateway_status() {
    python3 - "$business_port" "$jwt_secret" "$jwt_issuer" <<'PY'
import base64
import hashlib
import hmac
import http.client
import json
import sys
import time

def b64url(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")

port, secret, issuer = sys.argv[1:]
now = int(time.time())
header = b64url(json.dumps({"alg":"HS256","typ":"JWT"}, separators=(",", ":")).encode())
payload = b64url(json.dumps({"sub":"soak-control","iss":issuer,"iat":now,"exp":now+600,"scope":"read"}, separators=(",", ":")).encode())
signature = b64url(hmac.new(secret.encode(), f"{header}.{payload}".encode(), hashlib.sha256).digest())
conn = http.client.HTTPConnection("127.0.0.1", int(port), timeout=3)
conn.request("GET", "/api/control", headers={"Authorization": "Bearer " + f"{header}.{payload}.{signature}", "X-Forwarded-For": "198.18.250.1"})
response = conn.getresponse()
response.read()
print(response.status)
PY
}

start_keepalive_probe() {
    python3 - "$business_port" "$jwt_secret" "$jwt_issuer" <<'PY' >"$run_dir/keepalive.log" 2>&1 &
import base64
import hashlib
import hmac
import http.client
import json
import sys
import time

def b64url(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")

port, secret, issuer = sys.argv[1:]
now = int(time.time())
header = b64url(json.dumps({"alg":"HS256","typ":"JWT"}, separators=(",", ":")).encode())
payload = b64url(json.dumps({"sub":"soak-keepalive","iss":issuer,"iat":now,"exp":now+600,"scope":"read"}, separators=(",", ":")).encode())
signature = b64url(hmac.new(secret.encode(), f"{header}.{payload}".encode(), hashlib.sha256).digest())
conn = http.client.HTTPConnection("127.0.0.1", int(port), timeout=5)
conn.request("GET", "/api/keep", headers={"Authorization": "Bearer " + f"{header}.{payload}.{signature}", "X-Forwarded-For": "198.18.250.2", "Connection": "keep-alive"})
response = conn.getresponse()
response.read()
print("keepalive_status=" + str(response.status), flush=True)
time.sleep(30)
conn.close()
PY
    keepalive_pid=$!
}

start_slow_request() {
    gateway_status >"$run_dir/slow-request.log" 2>&1 &
    slow_pid=$!
}

# Fix 4: ASan binary 检测 — ASan shadow 内存本身占几百MB,64MB RSS 阈值对 ASan 构建毫无意义。
# 检测方式:文件名含 address/asan,或 readelf 发现 libasan 依赖。
if [[ "$gw_source" == *address* || "$gw_source" == *asan* ]] \
    || readelf -d "$gw_source" 2>/dev/null | grep -qi 'libasan\|libclang_rt.asan'; then
    is_asan_binary=1
    # 覆盖 RSS 预算:ASan shadow + interceptor 开销 ~500-800MB,给1GB 增长空间
    max_rss_growth_kb="${SOAK_MAX_RSS_GROWTH_KB:-1048576}"
    max_hub_rss_growth_kb="${SOAK_MAX_HUB_RSS_GROWTH_KB:-524288}"
fi

# Fix 5: banctl 必须用非 ASan binary — ASan+ucontext 协程在线程退出时 UnsetAlternateSignalStack
# munmap 失败导致 abort。默认优先用 build/bin/banctl(非 sanitized 构建),找不到再回退 bin/banctl。
if [[ -z "${API_GW_SOAK_BANCTL_BIN:-}" ]]; then
    if [[ -x "$repo/build/bin/banctl" ]]; then
        banctl_source="$repo/build/bin/banctl"
    fi
fi

[[ -x "$gw_source" ]] || die "missing ASan/UBSan gateway binary: $gw_source"
[[ -x "$hub_source" ]] || die "missing ASan/UBSan hub binary: $hub_source"
[[ -x "$banctl_source" ]] || die "missing banctl binary: $banctl_source"
[[ -f "$mock_py" && -f "$bench_py" && -f "$probes_py" ]] || die "missing soak support script"
command -v python3 >/dev/null || die "python3 is required"
command -v logrotate >/dev/null || die "logrotate is required for the production log-limit gate"
if [[ -n "$k6_duration" ]]; then
    command -v "$k6_bin" >/dev/null || die "k6 is required when SOAK_K6_DURATION is set"
fi
require_positive "$baseline_seconds" "$fault_seconds" "$rate_seconds" "$recovery_seconds" "$workers" \
    "$base_qps" "$burst_qps" "$multi_upstream_seconds" "$multi_upstream_qps" \
    "$multi_upstream_min_requests" "$fault_qps" "$rate_qps" "$client_ips" "$reload_seconds" "$log_reload_seconds" \
    "$monitor_seconds" "$term_timeout"

run_dir="$(mktemp -d "${TMPDIR:-/tmp}/bronx_api_gw_soak.XXXXXX")"
runtime_root="$run_dir/root"
{ read -r business_port; read -r admin_port; read -r upstream_a_port; read -r upstream_b_port; \
  read -r upstream_c_port; read -r hub_http_port; } < <(alloc_ports 6)
submit_sock="$runtime_root/submit.sock"
subscribe_sock="$runtime_root/subscribe.sock"
hub_admin_sock="$runtime_root/admin.sock"
mkdir -p "$runtime_root/bin" "$runtime_root/api_gw/bin" "$runtime_root/logs"
materialize_app "$gw_source" gw
materialize_app "$hub_source" hub
materialize_app "$banctl_source" banctl
write_framework_log_config "base" 0
write_hub_framework_config
write_gateway 10000 10000 2500 2500
write_hub

printf 'run_dir=%s\nroot=%s\nports business=%s admin=%s upstream_a=%s upstream_b=%s upstream_c=%s hub=%s\n' \
    "$run_dir" "$runtime_root" "$business_port" "$admin_port" "$upstream_a_port" "$upstream_b_port" \
    "$upstream_c_port" "$hub_http_port"

start_hub || {
    sed -n '1,240p' "$run_dir/hub.log" >&2 || true
    die "hub did not become ready"
}

python3 "$mock_py" --host 127.0.0.1 --port "$upstream_a_port" --name soak-a --seed "$seed" \
    >"$run_dir/mock-a.log" 2>&1 &
mock_a_pid=$!
python3 "$mock_py" --host 127.0.0.1 --port "$upstream_b_port" --name soak-b --seed "$seed" \
    >"$run_dir/mock-b.log" 2>&1 &
mock_b_pid=$!
python3 "$mock_py" --host 127.0.0.1 --port "$upstream_c_port" --name soak-c --seed "$seed" \
    >"$run_dir/mock-c.log" 2>&1 &
mock_c_pid=$!
wait_http "http://127.0.0.1:$upstream_a_port/_soak/stats" "$mock_a_pid" || die "mock upstream A did not become ready"
wait_http "http://127.0.0.1:$upstream_b_port/_soak/stats" "$mock_b_pid" || die "mock upstream B did not become ready"
wait_http "http://127.0.0.1:$upstream_c_port/_soak/stats" "$mock_c_pid" || die "mock upstream C did not become ready"

(
    cd "$runtime_root"
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:use_sigaltstack=0:log_path=$run_dir/asan-gw" \
    UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=0' exec "$runtime_root/bin/gw"
) >"$run_dir/gw.log" 2>&1 &
gw_pid=$!
wait_http "http://127.0.0.1:$admin_port/healthz" "$gw_pid" || {
    sed -n '1,240p' "$run_dir/gw.log" >&2 || true
    die "gateway did not become ready"
}
start_resource_monitor

if ! reset_mock_stats; then
    die "cannot configure healthy mock upstreams"
fi
if ! mock_control '{"chunked_probability":0.25}'; then
    die "cannot configure primary mock response framing"
fi
if ! assert_api_upstreams_healthy; then
    die "api upstream health checks did not settle"
fi
run_bench "multi-upstream" "$multi_upstream_seconds" "$workers" "$multi_upstream_qps" "$client_ips" \
    'normal=100,no_token=0,post=0,not_found=0,bad_token=0,disconnect=0' \
    --min-attempts "normal:$multi_upstream_min_requests" --min-status-ratio normal:200:0.99 --max-transport-errors 0
if ! assert_multi_upstream_load; then
    fail "round-robin multi-upstream load was not balanced"
fi

if ! run_ipban_probe full gateway-features.json; then
    fail "gateway feature probes"
fi

write_gateway 2 20 2500 2500 true 3 10000 30000
if ! admin_reload; then
    fail "cannot enable rate-report probe configuration"
elif ! run_ipban_probe rate ipban-rate.json; then
    fail "rate-limit IP-ban probe"
fi
write_gateway 10000 10000 2500 2500
if ! admin_reload; then
    fail "cannot restore normal rate-report configuration"
elif ! run_ipban_probe persist-seed ipban-persist-seed.json; then
    fail "IP-ban persistence seed probe"
fi
if ! stop_pid "resource monitor" "$monitor_pid" 3; then
    fail "resource monitor did not stop before hub restart"
fi
monitor_pid=""
if ! restart_hub; then
    fail "hub restart for persistence probe"
elif ! run_ipban_probe persist-verify ipban-persist-verify.json; then
    fail "IP-ban persistence recovery probe"
fi
start_resource_monitor

if [[ "$probe_only" == "1" ]]; then
    printf '\n=== gateway probe verdict ===\n'
    printf 'failures=%s artifacts=%s\n' "$failures" "$run_dir"
    if (( failures > 0 )); then
        exit 1
    fi
    printf 'PASS: gateway probes satisfied\n'
    exit 0
fi

if [[ -n "$k6_duration" ]]; then
    write_gateway 100 100 2500 2500
    if ! admin_reload; then
        fail "cannot apply k6 rate-limit configuration"
    else
        printf '\n=== k6 soak duration=%s rps=%s ===\n' "$k6_duration" "$k6_normal_rps"
        mkdir -p "$run_dir/k6-soak"
        if ! K6_GW_HOST=127.0.0.1 K6_GW_PORT="$business_port" \
            K6_ADMIN_HOST=127.0.0.1 K6_ADMIN_PORT="$admin_port" \
            K6_HUB_HOST=127.0.0.1 K6_HUB_PORT="$hub_http_port" \
            K6_NORMAL_RPS="$k6_normal_rps" K6_RATE_PROBE_INTERVAL=30s \
            SOAK_DURATION="$k6_duration" "$k6_bin" run \
            --summary-export "$run_dir/k6-soak/summary.json" \
            "$repo/test/k6/soak.js" 2>&1 | tee "$run_dir/k6-soak/k6.log"; then
            fail "k6 soak phase"
        fi
    fi
    write_gateway 10000 10000 2500 2500
    admin_reload || fail "cannot restore normal configuration after k6"
fi

write_framework_log_config "hup-a" 0
if ! signal_hup_expect_gw_log "hup-a" 1; then
    fail "SIGHUP did not hot-apply the Bronx logger configuration"
fi
if [[ "$(gateway_status)" != "200" ]]; then
    fail "gateway request failed after Bronx logger SIGHUP reload"
fi

(
    trap - EXIT INT TERM
    while pid_running "$gw_pid"; do
        sleep "$reload_seconds"
        write_gateway 10000 10000 2500 2500
        if ! admin_reload; then
            printf 'reload_http=non-200\n' >>"$run_dir/reload.log"
        else
            printf 'reload_http=200\n' >>"$run_dir/reload.log"
        fi
    done
) &
reload_pid=$!

(
    trap - EXIT INT TERM
    mode="hup-b"
    while pid_running "$gw_pid"; do
        sleep "$log_reload_seconds"
        next_count="$(log_count "$runtime_root/logs/gw-$mode.log" "framework config reloaded")"
        write_framework_log_config "$mode" 0
        if signal_hup_expect_gw_log "$mode" "$((next_count + 1))"; then
            printf 'hup=ok mode=%s\n' "$mode" >>"$run_dir/log-reload.log"
        else
            printf 'hup=failed mode=%s\n' "$mode" >>"$run_dir/log-reload.log"
        fi
        if [[ "$mode" == "hup-a" ]]; then
            mode="hup-b"
        else
            mode="hup-a"
        fi
    done
) &
log_reload_pid=$!

run_bench "baseline" "$baseline_seconds" "$workers" "$base_qps" "$client_ips" \
    'normal=60,no_token=15,post=10,not_found=8,bad_token=4,disconnect=3' \
    --min-attempts "normal:$min_scenario_attempts" --min-attempts "no_token:$min_scenario_attempts" \
    --min-attempts "post:$min_scenario_attempts" --min-attempts "not_found:$min_scenario_attempts" \
    --min-attempts "bad_token:$min_scenario_attempts" --min-attempts "disconnect:$min_scenario_attempts" \
    --min-status-ratio normal:200:0.99 --min-status-ratio no_token:401:0.99 \
    --min-status-ratio post:200:0.99 --min-status-ratio not_found:404:0.99 \
    --min-status-ratio bad_token:401:0.99 \
    --max-transport-errors "$(( baseline_seconds * base_qps * 3 / 100 / 10 ))"

run_bench "burst" "$fault_seconds" "$workers" "$burst_qps" "$client_ips" \
    'normal=100,no_token=0,post=0,not_found=0,bad_token=0,disconnect=0' \
    --min-attempts "normal:$min_scenario_attempts" --min-status-ratio normal:200:0.99 --max-transport-errors 0

if ! stop_pid "reload loop" "$reload_pid" 3; then fail "reload loop did not stop"; fi
reload_pid=""
if ! stop_pid "framework log reload loop" "$log_reload_pid" 3; then fail "framework log reload loop did not stop"; fi
log_reload_pid=""
if [[ -s "$run_dir/reload.log" ]] && grep -qv 'reload_http=200' "$run_dir/reload.log"; then
    fail "admin reload returned non-200 during baseline traffic"
fi
if [[ -s "$run_dir/log-reload.log" ]] && grep -qv 'hup=ok' "$run_dir/log-reload.log"; then
    fail "Bronx logger SIGHUP reload failed during baseline traffic"
fi
assert_active_zero "baseline"

exercise_log_sample_limit
exercise_log_rotation

write_gateway 4 1 2500 2500
admin_reload || fail "cannot apply low rate-limit configuration"
run_bench "rate-limit" "$rate_seconds" 4 "$rate_qps" 1 \
    'normal=100,no_token=0,post=0,not_found=0,bad_token=0,disconnect=0' \
    --min-attempts "normal:$min_scenario_attempts" --min-status-ratio normal:429:0.70 --max-transport-errors 0
snapshot_stats "rate-limit"
if ! json_at_least "$run_dir/stats-rate-limit.json" rate_limited 1; then fail "rate-limit metric did not grow"; fi

write_gateway 10000 10000 2500 2500
admin_reload || fail "cannot restore high rate-limit configuration"
run_bench "rate-recovery" "$recovery_seconds" 4 "$base_qps" "$client_ips" \
    'normal=100,no_token=0,post=0,not_found=0,bad_token=0,disconnect=0' \
    --min-attempts "normal:$min_scenario_attempts" --min-status-ratio normal:200:0.99 --max-transport-errors 0

snapshot_stats "before-circuit"
reset_mock_stats || fail "cannot reset mock upstreams before circuit test"
mock_control '{"error_probability":0.10,"error_burst":5,"drop_probability":0,"slow_probability":0,"reset_stats":true}' \
    || fail "cannot enable deterministic 503 bursts"
run_bench "circuit-fault" "$fault_seconds" "$workers" "$fault_qps" "$client_ips" \
    'normal=100,no_token=0,post=0,not_found=0,bad_token=0,disconnect=0' \
    --min-attempts "normal:$min_scenario_attempts" --max-transport-errors 0
snapshot_stats "after-circuit"
if ! json_delta_at_least "$run_dir/stats-before-circuit.json" "$run_dir/stats-after-circuit.json" upstream_fail 5; then
    fail "503 bursts did not increase upstream failure accounting"
fi
assert_circuit_opened || fail "503 bursts did not open the circuit"
assert_circuit_fault_isolated || fail "healthy upstreams did not carry traffic while A was faulting"

mock_control '{"error_probability":0,"drop_probability":0,"slow_probability":0,"reset_stats":true}' \
    || fail "cannot restore healthy mock after circuit test"
sleep 3
run_bench "circuit-recovery" "$recovery_seconds" 4 "$base_qps" "$client_ips" \
    'normal=100,no_token=0,post=0,not_found=0,bad_token=0,disconnect=0' \
    --min-attempts "normal:$min_scenario_attempts" --min-status-ratio normal:200:0.99 --max-transport-errors 0
assert_circuit_closed || fail "circuit did not close after a healthy half-open probe"

write_gateway 10000 10000 400 400
admin_reload || fail "cannot apply short upstream timeout configuration"
snapshot_stats "before-transport-fault"
mock_control '{"error_probability":0,"drop_probability":0.15,"slow_probability":0.15,"slow_delay_ms":1000,"reset_stats":true}' \
    || fail "cannot enable drop and timeout faults"
run_bench "transport-fault" "$fault_seconds" "$workers" "$fault_qps" "$client_ips" \
    'normal=100,no_token=0,post=0,not_found=0,bad_token=0,disconnect=0' \
    --min-attempts "normal:$min_scenario_attempts" --max-transport-errors 0
snapshot_stats "after-transport-fault"
if ! json_delta_at_least "$run_dir/stats-before-transport-fault.json" "$run_dir/stats-after-transport-fault.json" upstream_fail 1; then
    fail "drop and timeout faults did not increase upstream failure accounting"
fi

mock_control '{"error_probability":0,"drop_probability":0,"slow_probability":0,"reset_stats":true}' \
    || fail "cannot restore healthy mock after transport faults"
write_gateway 10000 10000 2500 2500
admin_reload || fail "cannot restore normal upstream deadlines"
run_bench "transport-recovery" "$recovery_seconds" 4 "$base_qps" "$client_ips" \
    'normal=100,no_token=0,post=0,not_found=0,bad_token=0,disconnect=0' \
    --min-attempts "normal:$min_scenario_attempts" --min-status-ratio normal:200:0.99 --max-transport-errors 0
assert_active_zero "pre-stop"

cp "$runtime_root/api_gw/bin/gateway.yml" "$run_dir/gateway.yml.bak"
printf 'server: [\n' >"$runtime_root/api_gw/bin/gateway.yml.tmp"
mv "$runtime_root/api_gw/bin/gateway.yml.tmp" "$runtime_root/api_gw/bin/gateway.yml"
if admin_reload; then
    fail "malformed gateway YAML was accepted by admin reload"
fi
if [[ "$(gateway_status)" != "200" ]]; then
    fail "last good config did not survive malformed reload"
fi
if ! assert_admin_routes; then
    fail "admin route view did not preserve the last good config"
fi
cp "$run_dir/gateway.yml.bak" "$runtime_root/api_gw/bin/gateway.yml.tmp"
mv "$runtime_root/api_gw/bin/gateway.yml.tmp" "$runtime_root/api_gw/bin/gateway.yml"
admin_reload || fail "cannot restore valid config after malformed reload"

mock_control '{"error_probability":0,"drop_probability":0,"slow_probability":0,"slow_delay_ms":1500,"reset_stats":true}' \
    || fail "cannot prepare graceful-stop fault"
start_keepalive_probe
sleep 0.4
mock_control '{"slow_probability":1,"slow_delay_ms":1500}' || fail "cannot start slow request fault"
start_slow_request
sleep 0.3
if ! stop_pid "gateway graceful shutdown" "$gw_pid" "$term_timeout"; then
    fail "gateway required SIGKILL during TERM with keep-alive and slow upstream"
fi
gw_pid=""
stop_pid "keep-alive probe" "$keepalive_pid" 3 || true
keepalive_pid=""
wait "$slow_pid" 2>/dev/null || true
slow_pid=""
stop_pid "resource monitor" "$monitor_pid" 3 || true
monitor_pid=""

if ! assert_resource_budget; then fail "resource budget"; fi

if grep -REn 'ERROR: (AddressSanitizer|UndefinedBehaviorSanitizer)|runtime error:|SUMMARY: (AddressSanitizer|UndefinedBehaviorSanitizer)' \
        "$run_dir"/asan-gw.* "$run_dir"/asan-hub.* "$run_dir/gw.log" "$run_dir/hub.log" 2>/dev/null; then
    fail "sanitizer report found"
fi
if grep -Eq 'CRASH|SIGSEGV|SIGABRT|Assertion|terminate called|double free|stack smashing' \
        "$run_dir/gw.log" "$run_dir/hub.log"; then
    fail "crash signature found in gateway or hub log"
fi

printf '\n=== production soak verdict ===\n'
printf 'failures=%s artifacts=%s\n' "$failures" "$run_dir"
if (( failures > 0 )); then
    exit 1
fi
printf 'PASS: production soak gates satisfied\n'
