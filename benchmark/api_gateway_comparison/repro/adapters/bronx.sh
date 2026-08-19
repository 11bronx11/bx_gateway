#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$script_dir/../../../.." && pwd)"
: "${BENCH_PRODUCT_DIR:?BENCH_PRODUCT_DIR is required}"
state="$BENCH_PRODUCT_DIR/adapter.state"

die() {
  printf 'bronx adapter: %s\n' "$*" >&2
  exit 1
}

load_state() {
  [[ -f "$state" ]] || die "state is missing; run prepare first"
  # State is generated only from validated paths and numeric ports in this adapter.
  source "$state"
}

unit_pid() {
  local unit="$1"
  systemctl --user show "$unit" -p MainPID --value 2>/dev/null | awk '$1 ~ /^[0-9]+$/ && $1 > 0 {print $1}'
}

wait_http() {
  local url="$1"
  local attempts="${2:-150}"
  local code
  for ((i = 0; i < attempts; ++i)); do
    code="$(curl -sS -o /dev/null -w '%{http_code}' --max-time 1 "$url" 2>/dev/null || true)"
    [[ "$code" == 2* ]] && return 0
    sleep 0.1
  done
  return 1
}

write_configs() {
  python3 - "$runtime" "$BENCH_GATEWAY_PORT" "$BENCH_ADMIN_PORT" "$BENCH_HUB_PORT" \
    "$BENCH_UPSTREAM_A_PORT" "$BENCH_UPSTREAM_B_PORT" "$BENCH_UPSTREAM_C_PORT" \
    "$BENCH_UPSTREAM_D_PORT" "$BENCH_UPSTREAM_E_PORT" "$BENCH_JWT_SECRET" \
    "$BENCH_JWT_ISSUER" "$BENCH_RATE_CAPACITY" "$BENCH_RATE_REFILL" \
    "$submit_sock" "$subscribe_sock" "$admin_sock" "${BENCH_UPSTREAM_COUNT:-5}" \
    "${BENCH_GATEWAY_WORKERS:-1}" "${BENCH_CAPACITY_MODE:-0}" "$BENCH_BANNED_IP" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
gateway_port, admin_port, hub_port, upstream_a, upstream_b, upstream_c, upstream_d, upstream_e = map(int, sys.argv[2:10])
secret, issuer = sys.argv[10:12]
capacity, refill = map(int, sys.argv[12:14])
submit_sock, subscribe_sock, admin_sock = sys.argv[14:17]
upstream_count, gateway_workers = map(int, sys.argv[17:19])
capacity_mode = sys.argv[19] == "1"
banned_ip = sys.argv[20]
config_dir = root / "api_gw" / "bin"

upstream_ports = [upstream_a, upstream_b, upstream_c, upstream_d, upstream_e][:upstream_count]
weights = [1, 1] if upstream_count == 2 else [5, 3, 2, 1, 1]
endpoint_text = "\n".join(
    f'      - {{ host: "127.0.0.1", port: {port}, weight: {weight} }}'
    for port, weight in zip(upstream_ports, weights)
)

routes = [
    {"name": "api", "type": "prefix", "path": "/api", "upstream": "api", "rate": True},
    {"name": "ws", "type": "prefix", "path": "/ws", "upstream": "ws", "rate": False},
    {"name": "ban", "type": "prefix", "path": "/probe/ban", "upstream": "api", "rate": False},
    {"name": "rate", "type": "prefix", "path": "/probe/rate", "upstream": "api", "rate": True},
    {"name": "observe", "type": "prefix", "path": "/probe/observe", "upstream": "api", "rate": True},
]
routes.extend({"name": f"exact-{index}", "type": "exact", "path": f"/exact/{index}", "upstream": "api", "rate": True}
              for index in range(1, 5))
routes.extend({"name": f"route-{index}", "type": "prefix", "path": f"/route/{index}", "upstream": "api", "rate": True}
              for index in range(1, 11))
route_text = []
for route in routes:
    route_text.extend([
        f"  - name: {route['name']}",
        f"    path: {route['path']}",
        f"    match_type: {route['type']}",
        f"    upstream: {route['upstream']}",
        "    strip_prefix: true",
        "    methods: [GET, POST]",
        "    auth: jwt",
        "    require_scopes: [read]",
        "    request_headers:",
        "      set:",
        "        X-Bench-Version: v1",
        "      remove: [X-Internal-Debug]",
        "    rate_limit:",
        f"      enabled: {'true' if route['rate'] else 'false'}",
        f"      capacity: {capacity}",
        f"      refill_per_sec: {refill}",
        "      key: ip",
    ])

gateway = f'''server:
  address: "127.0.0.1:{gateway_port}"
  admin_address: "127.0.0.1:{admin_port}"
  io_workers: {gateway_workers}
  maintenance: false
trusted_proxies:
  - 127.0.0.1/32
upstreams:
  - name: api
    lb: weighted_least_conn
    timeout:
      total_ms: 1500
      connect_ms: 300
      read_ms: 1000
    limits:
      max_inflight: 512
    circuit_breaker:
      failure_threshold: 3
      window_ms: 5000
      buckets: 5
      min_requests: 3
      failure_rate: 50
      slow_ms: 300
      slow_rate: 50
      open_timeout_ms: 3000
      max_open_timeout_ms: 8000
      half_open_max_requests: 2
      half_open_successes: 1
      failure_statuses: [500, 502, 503, 504]
    connection_pool:
      max_idle: 64
      idle_timeout_ms: 10000
    health_check:
      enabled: true
      path: /_soak/health
      interval_ms: 1000
      timeout_ms: 300
      healthy_threshold: 1
      unhealthy_threshold: 2
    endpoints:
{endpoint_text}
  - name: ws
    lb: round_robin
    timeout:
      total_ms: 5000
      connect_ms: 300
      read_ms: 5000
    connection_pool:
      max_idle: 64
      idle_timeout_ms: 10000
    endpoints:
      - {{ host: "127.0.0.1", port: {upstream_a}, weight: 1 }}
routes:
{chr(10).join(route_text)}
auth:
  jwt:
    enabled: true
    algo: HS256
    secret: {json.dumps(secret)}
    issuer: {json.dumps(issuer)}
    leeway_sec: 0
  api_key:
    header: X-API-Key
    keys: []
  forward:
    enabled: true
    strip_bearer: true
    strip_api_key: true
ip_filter:
  enabled: {'true' if capacity_mode else 'false'}
  mode: denylist
  cidrs: [{json.dumps(banned_ip)}]
ip_policy:
  submit_sock: "{submit_sock}"
  subscribe_sock: "{subscribe_sock}"
  instance_id: "bench-gw"
  waf:
    enabled: true
    ban_ms: 60000
  rate_report:
    enabled: false
    hits: 20
    window_ms: 10000
    ban_ms: 30000
'''

hub = f'''server:
  submit_sock: "{submit_sock}"
  subscribe_sock: "{subscribe_sock}"
  admin_sock: "{admin_sock}"
  http_address: "127.0.0.1:{hub_port}"
  db_path: "{root / 'api_gw' / 'bin' / 'hub.db'}"
  iom_workers: 1
'''

framework = f'''cpu_pool:
  threads: {gateway_workers}
  max_queue: 0
  name: bench-cpu
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
        file: logs/root.log
        async: true
  - name: system
    level: info
    appenders:
      - type: BxFileLogAppender
        file: logs/system.log
        async: true
  - name: gw
    level: info
    appenders:
      - type: BxFileLogAppender
        file: logs/gw.log
        async: true
  - name: trace
    level: info
    appenders:
      - type: BxFileLogAppender
        file: logs/trace.log
        async: true
'''

hub_framework = framework.replace(f"  threads: {gateway_workers}", "  threads: 1", 1) \
    .replace("name: bench-cpu", "name: bench-hub-cpu") \
    .replace("file: logs/gw.log", "file: logs/hub.log")
(config_dir / "gateway.yml").write_text(gateway, encoding="utf-8")
(config_dir / "hub.yml").write_text(hub, encoding="utf-8")
(config_dir / "bronx.yml").write_text(framework, encoding="utf-8")
(config_dir / "hub_bronx.yml").write_text(hub_framework, encoding="utf-8")
PY
}

start_unit() {
  local unit="$1"
  local binary="$2"
  local log="$3"
  systemd-run --user --quiet --collect --unit "$unit" --slice "$slice" \
    --working-directory "$runtime" \
    --property "StandardOutput=append:$log" \
    --property "StandardError=append:$log" \
    "$binary"
}

start_hub() {
  systemctl --user stop "$hub_unit" >/dev/null 2>&1 || true
  start_unit "$hub_unit" "$runtime/bin/hub" "$BENCH_PRODUCT_DIR/logs/hub-process.log"
  wait_http "http://127.0.0.1:$BENCH_HUB_PORT/healthz" 150
}

start_gateway() {
  systemctl --user stop "$gw_unit" >/dev/null 2>&1 || true
  start_unit "$gw_unit" "$runtime/bin/gw" "$BENCH_PRODUCT_DIR/logs/gw-process.log"
  wait_http "http://127.0.0.1:$BENCH_ADMIN_PORT/healthz" 200
}

snapshot() {
  local label="${1:-snapshot}"
  local out="$BENCH_PRODUCT_DIR/snapshots/$label"
  mkdir -p "$out"
  curl -sS --max-time 2 "http://127.0.0.1:$BENCH_ADMIN_PORT/stats" >"$out/stats.json" || true
  curl -sS --max-time 2 "http://127.0.0.1:$BENCH_ADMIN_PORT/routes" >"$out/routes.json" || true
  curl -sS --max-time 2 "http://127.0.0.1:$BENCH_ADMIN_PORT/metrics" >"$out/metrics.prom" || true
  curl -sS --max-time 2 "http://127.0.0.1:$BENCH_ADMIN_PORT/trace" >"$out/trace.json" || true
  curl -sS --max-time 2 "http://127.0.0.1:$BENCH_HUB_PORT/metrics" >"$out/hub-metrics.prom" || true
  "$runtime/bin/banctl" --sock "$admin_sock" --json list >"$out/ipban.json" 2>&1 || true
  systemctl --user show "$slice.slice" -p CPUQuotaPerSecUSec -p MemoryMax -p AllowedCPUs \
    -p ControlGroup >"$out/resource-limits.txt" 2>&1 || true
  systemctl --user show "$gw_unit" "$hub_unit" -p Id -p MainPID -p ExecStart \
    >"$out/services.txt" 2>&1 || true
}

action="${1:-}"
case "$action" in
  preflight)
    for path in "$repo/bin/gw" "$repo/bin/hub" "$repo/bin/banctl"; do
      [[ -x "$path" ]] || die "missing executable $path"
    done
    for command in python3 curl systemd-run systemctl; do
      command -v "$command" >/dev/null || die "missing command $command"
    done
    systemctl --user is-system-running >/dev/null 2>&1 || die "user systemd is not running"
    ;;
  prepare)
    case "${BENCH_UPSTREAM_COUNT:-5}" in 2|5) ;; *) die "BENCH_UPSTREAM_COUNT must be 2 or 5" ;; esac
    for name in BENCH_GATEWAY_PORT BENCH_ADMIN_PORT BENCH_HUB_PORT BENCH_UPSTREAM_A_PORT \
      BENCH_UPSTREAM_B_PORT BENCH_UPSTREAM_C_PORT BENCH_UPSTREAM_D_PORT BENCH_UPSTREAM_E_PORT \
      BENCH_JWT_SECRET BENCH_JWT_ISSUER BENCH_BANNED_IP BENCH_RATE_CAPACITY \
      BENCH_RATE_REFILL BENCH_UNIT_TOKEN; do
      [[ -n "${!name:-}" ]] || die "$name is required"
    done
    runtime="$BENCH_PRODUCT_DIR/runtime"
    unit_token="$(printf '%s' "$BENCH_PRODUCT_DIR" | sha256sum | cut -c1-12)"
    slice="benchcmp$unit_token"
    gw_unit="benchcmp$unit_token-gw.service"
    hub_unit="benchcmp$unit_token-hub.service"
    submit_sock="/tmp/bronx_cmp_${unit_token}_submit.sock"
    subscribe_sock="/tmp/bronx_cmp_${unit_token}_subscribe.sock"
    admin_sock="/tmp/bronx_cmp_${unit_token}_admin.sock"
    for sock in "$submit_sock" "$subscribe_sock" "$admin_sock"; do
      [[ ! -e "$sock" ]] || die "refusing to reuse existing socket path $sock"
    done
    mkdir -p "$runtime/bin" "$runtime/api_gw/bin" "$runtime/logs" "$runtime/run" \
      "$BENCH_PRODUCT_DIR/logs" "$BENCH_PRODUCT_DIR/snapshots" "$BENCH_PRODUCT_DIR/configs"
    cp "$repo/bin/gw" "$runtime/bin/gw"
    cp "$repo/bin/hub" "$runtime/bin/hub"
    cp "$repo/bin/banctl" "$runtime/bin/banctl"
    write_configs
    cp "$runtime/api_gw/bin/"*.yml "$BENCH_PRODUCT_DIR/configs/"
    cat >"$BENCH_PRODUCT_DIR/capabilities.json" <<'JSON'
{
  "http_proxy": "PASS-INTERNAL",
  "websocket": "PASS-INTERNAL",
  "metrics": "PASS-INTERNAL",
  "routing": "PASS-INTERNAL",
  "jwt": "PASS-INTERNAL",
  "waf": "PASS-INTERNAL",
  "ip_ban": "PASS-INTERNAL",
  "rate_limit": "PASS-INTERNAL-TOKEN-BUCKET",
  "hot_reload": "PASS-INTERNAL",
  "high_precision_trace_toggle": "PASS-INTERNAL",
  "external_control_plane": "HUB-SQLITE"
}
JSON
    cat >"$state" <<EOF
runtime=$(printf '%q' "$runtime")
slice=$(printf '%q' "$slice")
gw_unit=$(printf '%q' "$gw_unit")
hub_unit=$(printf '%q' "$hub_unit")
submit_sock=$(printf '%q' "$submit_sock")
subscribe_sock=$(printf '%q' "$subscribe_sock")
admin_sock=$(printf '%q' "$admin_sock")
EOF
    ;;
  start)
    load_state
    systemctl --user set-property --runtime "$slice.slice" \
      CPUQuota="${BENCH_CPU_QUOTA_PERCENT:-100}%" \
      MemoryMax="${BENCH_MEMORY_BYTES:?}" AllowedCPUs="${BENCH_TARGET_CPU:-2}"
    start_hub || die "hub did not become ready"
    start_gateway || die "gateway did not become ready"
    systemctl --user show "$slice.slice" -p ControlGroup --value >"$BENCH_PRODUCT_DIR/cgroup.path"
    snapshot started
    ;;
  ready)
    load_state
    wait_http "http://127.0.0.1:$BENCH_ADMIN_PORT/healthz" 30
    wait_http "http://127.0.0.1:$BENCH_HUB_PORT/healthz" 30
    ;;
  reset-after-warmup)
    load_state
    printf '{"supported":false,"status":"NOOP","reason":"warmup has no injected faults; Bronx breaker starts closed"}\n' \
      >"$BENCH_PRODUCT_DIR/snapshots/reset-after-warmup.json"
    ;;
  pids)
    load_state
    unit_pid "$gw_unit"
    unit_pid "$hub_unit"
    ;;
  reload-valid)
    load_state
    sed -i 's/X-Bench-Version: v1/X-Bench-Version: v2/g' "$runtime/api_gw/bin/gateway.yml"
    curl -fsS --max-time 3 -X POST "http://127.0.0.1:$BENCH_ADMIN_PORT/reload" \
      >"$BENCH_PRODUCT_DIR/snapshots/reload-valid.txt"
    snapshot reload-valid
    ;;
  reload-invalid)
    load_state
    cp "$runtime/api_gw/bin/gateway.yml" "$runtime/api_gw/bin/gateway.yml.good"
    printf '\nroutes: [\n' >>"$runtime/api_gw/bin/gateway.yml"
    code="$(curl -sS --max-time 3 -o "$BENCH_PRODUCT_DIR/snapshots/reload-invalid.txt" \
      -w '%{http_code}' -X POST "http://127.0.0.1:$BENCH_ADMIN_PORT/reload" || true)"
    mv "$runtime/api_gw/bin/gateway.yml.good" "$runtime/api_gw/bin/gateway.yml"
    [[ "$code" != 2* ]] || die "invalid configuration was accepted"
    wait_http "http://127.0.0.1:$BENCH_ADMIN_PORT/healthz" 30
    snapshot reload-invalid
    ;;
  restart-control)
    load_state
    systemctl --user stop "$hub_unit"
    start_hub || die "hub did not recover"
    snapshot control-restarted
    ;;
  security-seed)
    load_state
    "$runtime/bin/banctl" --sock "$admin_sock" --json deny \
      "${BENCH_BANNED_IP:-198.18.40.40}" 10m bench-manual \
      >"$BENCH_PRODUCT_DIR/snapshots/security-seed.json"
    ;;
  security-unban)
    load_state
    "$runtime/bin/banctl" --sock "$admin_sock" --json unban \
      "${BENCH_BANNED_IP:-198.18.40.40}" \
      >"$BENCH_PRODUCT_DIR/snapshots/security-unban.json"
    ;;
  trace-on)
    load_state
    curl -fsS --max-time 3 -X POST \
      "http://127.0.0.1:$BENCH_ADMIN_PORT/trace/on?level=1&path=/probe/observe&sample=1&ttl=45" \
      >"$BENCH_PRODUCT_DIR/snapshots/trace-on.txt"
    snapshot trace-on
    ;;
  trace-off)
    load_state
    curl -fsS --max-time 3 -X POST "http://127.0.0.1:$BENCH_ADMIN_PORT/trace/off" \
      >"$BENCH_PRODUCT_DIR/snapshots/trace-off.txt"
    snapshot trace-off
    ;;
  observe-probe)
    load_state
    code="$(curl -sS --max-time 3 -o "$BENCH_PRODUCT_DIR/snapshots/observe-body.json" \
      -D "$BENCH_PRODUCT_DIR/snapshots/observe-headers.txt" -w '%{http_code}' \
      -H "Authorization: Bearer $BENCH_VALID_TOKEN" \
      -H 'X-Forwarded-For: 198.18.80.80' -H 'X-Request-ID: bench-observe-fixed' \
      "$BENCH_GATEWAY_URL/probe/observe")"
    [[ "$code" == 200 ]] || die "observation probe returned HTTP $code"
    ;;
  observe-verify)
    load_state
    sleep 5
    grep -F '"request_id":"bench-observe-fixed"' "$runtime/logs/system.log" \
      >"$BENCH_PRODUCT_DIR/snapshots/observe-system.log"
    grep -F 'GET /probe/observe' "$runtime/logs/trace.log" \
      >"$BENCH_PRODUCT_DIR/snapshots/observe-trace-path.log"
    grep -E 'req  head|chain start|up   acquire|up   req|up   rsp|bodyend|req  done' \
      "$runtime/logs/trace.log" >"$BENCH_PRODUCT_DIR/snapshots/observe-trace-stages.log"
    for stage in 'req  head' 'chain start' 'up   acquire' 'up   req' 'up   rsp' 'req  done'; do
      grep -F "$stage" "$BENCH_PRODUCT_DIR/snapshots/observe-trace-stages.log" >/dev/null \
        || die "trace stage missing: $stage"
    done
    snapshot observe-verified
    ;;
  log-rotate)
    load_state
    gw_pid="$(unit_pid "$gw_unit")"
    for log in "$runtime/logs/gw.log" "$runtime/logs/root.log"; do
      if [[ -f "$log" ]]; then
        mv "$log" "$log.1"
        : >"$log"
      fi
    done
    [[ -n "$gw_pid" ]] && kill -HUP "$gw_pid"
    sleep 0.5
    snapshot log-rotated
    ;;
  snapshot*)
    load_state
    snapshot "$action"
    ;;
  stop)
    if [[ -f "$state" ]]; then
      load_state
      curl -sS --max-time 1 -X POST "http://127.0.0.1:$BENCH_ADMIN_PORT/trace/off" >/dev/null 2>&1 || true
      systemctl --user stop "$gw_unit" "$hub_unit" >/dev/null 2>&1 || true
      systemctl --user stop "$slice.slice" >/dev/null 2>&1 || true
      systemctl --user reset-failed "$gw_unit" "$hub_unit" "$slice.slice" >/dev/null 2>&1 || true
      rm -f -- "$submit_sock" "$subscribe_sock" "$admin_sock"
    fi
    ;;
  *)
    die "unknown action: $action"
    ;;
esac
