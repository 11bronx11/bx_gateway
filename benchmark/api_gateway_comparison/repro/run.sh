#!/usr/bin/env bash
set -Eeuo pipefail

suite_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$suite_dir/../../.." && pwd)"
case_name="full-5m"
products_override=""
results_root="$suite_dir/results"
seed="${BENCH_SEED:-20260818}"
target_cpu="${BENCH_TARGET_CPU:-2}"
save_raw="${BENCH_SAVE_RAW:-1}"
profile="${BENCH_PROFILE:-512m}"

usage() {
  cat <<'EOF'
Usage: ./run.sh [options]

Options:
  --case NAME          Case file under cases/ without .json (default: full-5m)
  --products CSV       Serial product list override
  --results-root PATH  Evidence root (default: ./results)
  --seed INTEGER       Reproducible product-order seed
  --target-cpu LIST    CPU allowed for the tested product (default: 2)
  --profile NAME       Single-core memory profile: 512m, 1g or 2g (default: 512m)
  --no-raw             Do not retain per-sample k6 JSON output
  -h, --help           Show this help

Examples:
  ./run.sh --case smoke-20s --products bronx
  ./run.sh --case full-5m
EOF
}

while (($#)); do
  case "$1" in
    --case) case_name="${2:?missing --case value}"; shift 2 ;;
    --products) products_override="${2:?missing --products value}"; shift 2 ;;
    --results-root) results_root="${2:?missing --results-root value}"; shift 2 ;;
    --seed) seed="${2:?missing --seed value}"; shift 2 ;;
    --target-cpu) target_cpu="${2:?missing --target-cpu value}"; shift 2 ;;
    --profile) profile="${2:?missing --profile value}"; shift 2 ;;
    --no-raw) save_raw=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

case_file="$suite_dir/cases/$case_name.json"
[[ -f "$case_file" ]] || { printf 'case not found: %s\n' "$case_file" >&2; exit 2; }
for command in python3 jq curl k6 vegeta sha256sum gzip; do
  command -v "$command" >/dev/null 2>&1 || { printf 'missing command: %s\n' "$command" >&2; exit 2; }
done
[[ "$seed" =~ ^[0-9]+$ ]] || { printf 'seed must be an integer\n' >&2; exit 2; }
[[ "$save_raw" == 0 || "$save_raw" == 1 ]] || { printf 'BENCH_SAVE_RAW must be 0 or 1\n' >&2; exit 2; }
case "$profile" in
  512m)
    memory_bytes=536870912; kong_memory_bytes=402653184; pg_memory_bytes=134217728
    spring_xmx_mb=300; spring_direct_mb=96 ;;
  1g)
    memory_bytes=1073741824; kong_memory_bytes=805306368; pg_memory_bytes=268435456
    spring_xmx_mb=640; spring_direct_mb=192 ;;
  2g)
    memory_bytes=2147483648; kong_memory_bytes=1610612736; pg_memory_bytes=536870912
    spring_xmx_mb=1280; spring_direct_mb=384 ;;
  *) printf 'profile must be one of: 512m, 1g, 2g\n' >&2; exit 2 ;;
esac

duration_s="$(jq -er '.duration_s | numbers' "$case_file")"
warmup_s="$(jq -er '.warmup_s | numbers' "$case_file")"
ws_clients="$(jq -er '.websocket_clients | numbers' "$case_file")"
rate_capacity="$(jq -er '.rate_limit.capacity | numbers' "$case_file")"
rate_refill="$(jq -er '.rate_limit.refill_per_sec | numbers' "$case_file")"
semantic_min="$(jq -er '.semantic_min | numbers' "$case_file")"
fault_available_min="$(jq -er '.fault_available_min | numbers' "$case_file")"
recovery_available_min="$(jq -er '.recovery_available_min | numbers' "$case_file")"
stage_seconds="$(jq -er '.stage_seconds | numbers' "$case_file")"
stage_rates="$(jq -c '.stage_rps' "$case_file")"
traffic_weights="$(jq -c '.traffic_weights' "$case_file")"
warmup_rps="$(jq -r '.stage_rps[0]' "$case_file")"
max_rps="$(jq -r '.stage_rps | max' "$case_file")"
reload_start_s="$(jq -er '[.events[] | select(.action == "reload-valid") | .at_s][0] | numbers' "$case_file")"

if [[ -n "$products_override" ]]; then
  IFS=, read -r -a requested_products <<<"$products_override"
else
  mapfile -t requested_products < <(jq -r '.products[]' "$case_file")
fi
((${#requested_products[@]} > 0)) || { printf 'no products selected\n' >&2; exit 2; }

run_id="$(date -u +%Y%m%dT%H%M%SZ)-$(printf '%04x' "$((RANDOM & 65535))")"
run_root="$(python3 - "$results_root" "$run_id" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1]).expanduser().resolve()
target = root / sys.argv[2]
target.mkdir(parents=True, exist_ok=False)
print(target)
PY
)"
mkdir -p "$run_root/host" "$run_root/products" "$run_root/source"
cp "$case_file" "$run_root/case.json"
exec > >(tee -a "$run_root/run.log") 2>&1

current_adapter=""
current_product_dir=""
k6_pid=""
sampler_pid=""
mock_pids=()
product_failed=0
suite_failed=0
finished=0

pid_running() {
  local pid="${1:-}"
  [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null
}

stop_pid() {
  local pid="${1:-}"
  local label="${2:-process}"
  [[ -n "$pid" ]] || return 0
  if pid_running "$pid"; then
    kill -TERM "$pid" 2>/dev/null || true
    for _ in {1..30}; do
      pid_running "$pid" || break
      sleep 0.1
    done
    if pid_running "$pid"; then
      printf 'forcing stop of %s pid=%s\n' "$label" "$pid" >&2
      kill -KILL "$pid" 2>/dev/null || true
    fi
  fi
  wait "$pid" 2>/dev/null || true
}

refresh_pids() {
  [[ -n "$current_adapter" && -n "$current_product_dir" ]] || return 0
  "$current_adapter" pids >"$current_product_dir/pids.current.tmp" 2>/dev/null || true
  mv "$current_product_dir/pids.current.tmp" "$current_product_dir/pids.current"
}

cleanup_product() {
  stop_pid "$k6_pid" k6
  stop_pid "$sampler_pid" resource-sampler
  k6_pid=""
  sampler_pid=""
  if [[ -n "$current_adapter" && -n "$current_product_dir" ]]; then
    "$current_adapter" stop >>"$current_product_dir/adapter-stop.log" 2>&1 || product_failed=1
  fi
  local index=0
  for pid in "${mock_pids[@]}"; do
    stop_pid "$pid" "mock-$index"
    index=$((index + 1))
  done
  mock_pids=()
  current_adapter=""
  current_product_dir=""
}

compress_product_logs() {
  local product_dir="$1"
  [[ -d "$product_dir/logs" ]] || return 0
  /usr/bin/find "$product_dir/logs" -maxdepth 1 -type f -size +1M ! -name '*.gz' -print0 \
    | xargs -0 -r gzip -9
}

finalize() {
  local status=$?
  local manifest_status=0
  trap - EXIT INT TERM
  cleanup_product || true
  if (( !finished )); then
    printf 'ABORTED\n' >"$run_root/status.txt"
  fi
  printf 'evidence_dir=%s\n' "$run_root"
  if ! (
    cd "$run_root"
    /usr/bin/find . -type f ! -name manifest.sha256 -print0 | sort -z | xargs -0 sha256sum >manifest.sha256
  ); then
    manifest_status=1
    printf 'FAILED\n' >"$run_root/status.txt"
  fi
  ((manifest_status == 0)) || status=1
  if (( status != 0 )); then exit "$status"; fi
  exit "$suite_failed"
}
trap finalize EXIT
trap 'exit 130' INT TERM

capture_metadata() {
  local relative destination
  local source_files=(
    benchmark/api_gateway_comparison/TEST_DESIGN.md
    benchmark/api_gateway_comparison/repro/README.md
    benchmark/api_gateway_comparison/repro/run.sh
    benchmark/api_gateway_comparison/repro/cases/full-5m.json
    benchmark/api_gateway_comparison/repro/cases/smoke-20s.json
    benchmark/api_gateway_comparison/repro/load/full.js
    benchmark/api_gateway_comparison/repro/lib/resource_sampler.py
    benchmark/api_gateway_comparison/repro/lib/summarize.py
    benchmark/api_gateway_comparison/repro/adapters/bronx.sh
    benchmark/api_gateway_comparison/repro/adapters/spring.sh
    benchmark/api_gateway_comparison/repro/adapters/kong-dbless.sh
    benchmark/api_gateway_comparison/repro/adapters/kong-postgres.sh
    benchmark/api_gateway_comparison/repro/products/spring/pom.xml
    benchmark/api_gateway_comparison/repro/products/spring/application.yml.in
    benchmark/api_gateway_comparison/repro/products/spring/src/main/java/io/bronx/benchmark/GatewayApplication.java
    benchmark/api_gateway_comparison/repro/products/kong/render.py
    benchmark/api_gateway_comparison/repro/products/kong/plugins/bench-security/handler.lua
    benchmark/api_gateway_comparison/repro/products/kong/plugins/bench-security/schema.lua
    test/api_gw/mock_rich.py
  )
  {
    printf 'run_id=%s\n' "$run_id"
    printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'case=%s\n' "$case_name"
    printf 'seed=%s\n' "$seed"
    printf 'target_cpu=%s\n' "$target_cpu"
    printf 'profile=%s\n' "$profile"
    printf 'memory_bytes=%s\n' "$memory_bytes"
    printf 'duration_s=%s\n' "$duration_s"
    printf 'warmup_s=%s\n' "$warmup_s"
    printf 'stage_rps=%s\n' "$stage_rates"
    printf 'websocket_clients=%s\n' "$ws_clients"
    printf 'git_head=%s\n' "$(git -C "$repo" rev-parse HEAD)"
    printf 'kernel=%s\n' "$(uname -srmo)"
    printf 'k6=%s\n' "$(k6 version | head -1)"
    printf 'vegeta=%s\n' "$(vegeta -version 2>&1 | head -1)"
    printf 'docker=%s\n' "$(docker version --format '{{.Server.Version}}' 2>/dev/null || printf unavailable)"
  } >"$run_root/metadata.env"
  git -C "$repo" status --short >"$run_root/host/git-status.txt"
  git -C "$repo" diff --binary >"$run_root/host/worktree.patch"
  uname -a >"$run_root/host/uname.txt"
  lscpu >"$run_root/host/lscpu.txt" 2>&1 || true
  cat /proc/meminfo >"$run_root/host/meminfo.txt"
  cat /proc/cmdline >"$run_root/host/cmdline.txt"
  for relative in "${source_files[@]}"; do
    [[ -f "$repo/$relative" ]] || continue
    destination="$run_root/source/$relative"
    mkdir -p "$(dirname "$destination")"
    cp "$repo/$relative" "$destination"
  done
  (
    cd "$run_root/source"
    /usr/bin/find . -type f -print0 | sort -z | xargs -0 sha256sum >../source.sha256
  )
}

allocate_ports() {
  python3 - "$1" <<'PY'
import socket, sys
sockets = []
try:
    for _ in range(int(sys.argv[1])):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        print(sock.getsockname()[1])
finally:
    for sock in sockets:
        sock.close()
PY
}

wait_url() {
  local url="$1"
  local pid="$2"
  for _ in {1..100}; do
    pid_running "$pid" || return 1
    curl -fsS --max-time 1 "$url" >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  return 1
}

mock_control() {
  local upstream="$1" payload="$2" variable
  variable="BENCH_UPSTREAM_${upstream}_PORT"
  curl -fsS --max-time 3 -X POST -H 'Content-Type: application/json' -d "$payload" \
    "http://127.0.0.1:${!variable}/_soak/control" >/dev/null
}

snapshot_mocks() {
  local label="$1" upstream variable
  mkdir -p "$current_product_dir/snapshots/$label"
  for upstream in A B C D E; do
    variable="BENCH_UPSTREAM_${upstream}_PORT"
    curl -fsS --max-time 3 "http://127.0.0.1:${!variable}/_soak/stats" \
      >"$current_product_dir/snapshots/$label/upstream-${upstream,,}.json"
  done
}

record_event() {
  local at_s="$1" action="$2" ok="$3" started="$4" ended="$5" detail="$6"
  local started_ms="$7" ended_ms="$8" anchor_ms="$9"
  python3 - "$current_product_dir/events.jsonl" "$at_s" "$action" "$ok" "$started" "$ended" \
    "$detail" "$started_ms" "$ended_ms" "$anchor_ms" <<'PY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
started_ms, ended_ms, anchor_ms = map(int, sys.argv[8:11])
entry = {
    "scheduled_s": int(sys.argv[2]),
    "action": sys.argv[3],
    "ok": sys.argv[4] == "1",
    "started_utc": sys.argv[5],
    "ended_utc": sys.argv[6],
    "detail_file": sys.argv[7],
    "started_epoch_ms": started_ms,
    "ended_epoch_ms": ended_ms,
    "actual_offset_ms": started_ms - anchor_ms,
    "schedule_lag_ms": started_ms - anchor_ms - int(sys.argv[2]) * 1000,
}
with path.open("a", encoding="utf-8") as stream:
    stream.write(json.dumps(entry, sort_keys=True) + "\n")
PY
}

execute_event() {
  local at_s="$1" action="$2" anchor_ms="$3"
  local started ended started_ms ended_ms rc=0 upstream_event=0
  local detail="$current_product_dir/events/${at_s}-${action}.log"
  mkdir -p "$current_product_dir/events"
  started_ms="$(date +%s%3N)"
  started="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  case "$action" in
    upstream-c-503-on) upstream_event=1; mock_control C '{"error_probability":1,"error_burst":10}' >"$detail" 2>&1 || rc=$? ;;
    upstream-c-clear) upstream_event=1; mock_control C '{"error_probability":0,"clear_pending_errors":true,"healthy":true}' >"$detail" 2>&1 || rc=$? ;;
    upstream-d-slow-on) upstream_event=1; mock_control D '{"slow_probability":1,"slow_delay_ms":800}' >"$detail" 2>&1 || rc=$? ;;
    upstream-d-clear) upstream_event=1; mock_control D '{"slow_probability":0,"healthy":true}' >"$detail" 2>&1 || rc=$? ;;
    upstream-e-offline-on) upstream_event=1; mock_control E '{"healthy":false,"drop_probability":1}' >"$detail" 2>&1 || rc=$? ;;
    upstream-e-clear) upstream_event=1; mock_control E '{"healthy":true,"drop_probability":0,"clear_pending_errors":true}' >"$detail" 2>&1 || rc=$? ;;
    *) "$current_adapter" "$action" >"$detail" 2>&1 || rc=$? ;;
  esac
  refresh_pids
  snapshot_mocks "event-${at_s}-${action}" >>"$detail" 2>&1 || rc=$?
  if ((upstream_event)); then
    "$current_adapter" "snapshot-event-${at_s}-${action}" >>"$detail" 2>&1 || rc=$?
  fi
  ended_ms="$(date +%s%3N)"
  ended="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  if ((rc == 0)); then
    record_event "$at_s" "$action" 1 "$started" "$ended" "events/${at_s}-${action}.log" \
      "$started_ms" "$ended_ms" "$anchor_ms"
  else
    product_failed=1
    record_event "$at_s" "$action" 0 "$started" "$ended" "events/${at_s}-${action}.log" \
      "$started_ms" "$ended_ms" "$anchor_ms"
    printf 'event failed product=%s at=%s action=%s rc=%s\n' "$BENCH_PRODUCT" "$at_s" "$action" "$rc" >&2
  fi
}

run_events() {
  local anchor_ms="$1" at action target_ms now_ms wait_ms
  while IFS=$'\t' read -r at action; do
    target_ms=$((anchor_ms + at * 1000))
    while true; do
      now_ms="$(date +%s%3N)"
      wait_ms=$((target_ms - now_ms))
      ((wait_ms > 0)) || break
      sleep "$(awk -v ms="$wait_ms" 'BEGIN {printf "%.3f", ms / 1000}')"
    done
    execute_event "$at" "$action" "$anchor_ms"
  done < <(jq -r '.events | sort_by(.at_s)[] | [.at_s, .action] | @tsv' "$case_file")
}

run_k6() {
  local mode="$1" seconds="$2" out_prefix="$3"
  local raw_arg=()
  if [[ "$save_raw" == 1 && "$mode" == formal ]]; then
    raw_arg=(--out "json=$current_product_dir/raw/k6-metrics.json")
  fi
  BENCH_MODE="$mode" BENCH_DURATION_S="$seconds" \
  BENCH_GATEWAY_URL="$BENCH_GATEWAY_URL" BENCH_WS_URL="$BENCH_WS_URL" \
  BENCH_JWT_SECRET="$BENCH_JWT_SECRET" BENCH_JWT_ISSUER="$BENCH_JWT_ISSUER" \
  BENCH_BANNED_IP="$BENCH_BANNED_IP" BENCH_RATE_IP="$BENCH_RATE_IP" \
  BENCH_STAGE_RATES="$BENCH_STAGE_RATES" BENCH_STAGE_SECONDS="$BENCH_STAGE_SECONDS" \
  BENCH_TRAFFIC_WEIGHTS="$BENCH_TRAFFIC_WEIGHTS" BENCH_WARMUP_RPS="$BENCH_WARMUP_RPS" \
  BENCH_WS_CLIENTS="$BENCH_WS_CLIENTS" BENCH_FAULT_WINDOWS="$BENCH_FAULT_WINDOWS" \
  BENCH_BAN_TIMELINE="$BENCH_BAN_TIMELINE" \
  BENCH_SEMANTIC_MIN="$BENCH_SEMANTIC_MIN" BENCH_START_EPOCH_MS="${BENCH_START_EPOCH_MS:-0}" \
  BENCH_RELOAD_START_S="$BENCH_RELOAD_START_S" BENCH_CONTROL_GRACE_S=5 \
  BENCH_FAULT_AVAILABLE_MIN="$BENCH_FAULT_AVAILABLE_MIN" \
  BENCH_RECOVERY_AVAILABLE_MIN="$BENCH_RECOVERY_AVAILABLE_MIN" \
  k6 run --summary-export "$current_product_dir/raw/$out_prefix-summary.json" \
    "${raw_arg[@]}" "$suite_dir/load/full.js" >"$current_product_dir/logs/$out_prefix-k6.log" 2>&1
}

direct_calibration() {
  local rate=$((max_rps * 2)) upstream variable
  ((rate < 20)) && rate=20
  rate=$((rate / 5))
  for upstream in A B C D E; do
    variable="BENCH_UPSTREAM_${upstream}_PORT"
    printf 'GET http://127.0.0.1:%s/api/items\n' "${!variable}" \
      | vegeta attack -rate "$rate" -duration 2s -timeout 2s \
        >"$current_product_dir/raw/direct-${upstream,,}.bin"
    vegeta report -type=json "$current_product_dir/raw/direct-${upstream,,}.bin" \
      >"$current_product_dir/raw/direct-${upstream,,}-summary.json"
    jq -e '.success >= 0.99' "$current_product_dir/raw/direct-${upstream,,}-summary.json" >/dev/null
  done
}

run_product() {
  local product="$1"
  product_failed=0
  export BENCH_PRODUCT="$product"
  current_product_dir="$run_root/products/$product"
  current_adapter="$suite_dir/adapters/$product.sh"
  mkdir -p "$current_product_dir"/{configs,events,logs,raw,snapshots}
  : >"$current_product_dir/events.jsonl"
  printf 'RUNNING\n' >"$current_product_dir/status.txt"
  if [[ ! -x "$current_adapter" ]]; then
    printf 'missing executable adapter: %s\n' "$current_adapter" | tee "$current_product_dir/error.txt"
    printf 'FAILED\n' >"$current_product_dir/status.txt"
    current_adapter=""
    current_product_dir=""
    return 1
  fi

  mapfile -t ports < <(allocate_ports 8)
  export BENCH_GATEWAY_PORT="${ports[0]}"
  export BENCH_ADMIN_PORT="${ports[1]}"
  export BENCH_HUB_PORT="${ports[2]}"
  export BENCH_UPSTREAM_A_PORT="${ports[3]}"
  export BENCH_UPSTREAM_B_PORT="${ports[4]}"
  export BENCH_UPSTREAM_C_PORT="${ports[5]}"
  export BENCH_UPSTREAM_D_PORT="${ports[6]}"
  export BENCH_UPSTREAM_E_PORT="${ports[7]}"
  export BENCH_GATEWAY_URL="http://127.0.0.1:$BENCH_GATEWAY_PORT"
  export BENCH_WS_URL="ws://127.0.0.1:$BENCH_GATEWAY_PORT"
  export BENCH_JWT_SECRET="bench-secret-20260818-32-bytes-minimum"
  export BENCH_JWT_ISSUER="bench-issuer"
  export BENCH_VALID_TOKEN="$(python3 - "$BENCH_JWT_SECRET" "$BENCH_JWT_ISSUER" <<'PY'
import base64, hashlib, hmac, json, sys, time
enc = lambda value: base64.urlsafe_b64encode(value).rstrip(b"=").decode()
header = enc(json.dumps({"alg":"HS256","typ":"JWT"}, separators=(",", ":")).encode())
body = enc(json.dumps({"iss":sys.argv[2],"sub":"bench-observe","scope":"read","exp":int(time.time())+3600}, separators=(",", ":")).encode())
sig = enc(hmac.new(sys.argv[1].encode(), f"{header}.{body}".encode(), hashlib.sha256).digest())
print(f"{header}.{body}.{sig}")
PY
)"
  export BENCH_BANNED_IP="198.18.40.40"
  export BENCH_RATE_IP="198.18.50.50"
  export BENCH_RATE_CAPACITY="$rate_capacity"
  export BENCH_RATE_REFILL="$rate_refill"
  export BENCH_TARGET_CPU="$target_cpu"
  export BENCH_MEMORY_BYTES="$memory_bytes"
  export BENCH_KONG_MEMORY_BYTES="$kong_memory_bytes"
  export BENCH_PG_MEMORY_BYTES="$pg_memory_bytes"
  export BENCH_SPRING_XMX_MB="$spring_xmx_mb"
  export BENCH_SPRING_DIRECT_MB="$spring_direct_mb"
  export BENCH_UNIT_TOKEN="${run_id:0:20}-${product//[^a-zA-Z0-9]/-}"
  export BENCH_STAGE_RATES="$stage_rates" BENCH_STAGE_SECONDS="$stage_seconds"
  export BENCH_TRAFFIC_WEIGHTS="$traffic_weights" BENCH_WARMUP_RPS="$warmup_rps"
  export BENCH_WS_CLIENTS="$ws_clients"
  export BENCH_FAULT_WINDOWS="$(jq -c '
    def at($name): [.events[] | select(.action == $name) | .at_s][0];
    [{name:"c_503",start_s:at("upstream-c-503-on"),end_s:at("upstream-c-clear")},
     {name:"d_slow",start_s:at("upstream-d-slow-on"),end_s:at("upstream-d-clear")},
     {name:"e_offline",start_s:at("upstream-e-offline-on"),end_s:at("upstream-e-clear")}]' "$case_file")"
  export BENCH_BAN_TIMELINE="$(jq -c '
    [{at_s:0,banned:true}] +
    [.events[] | select(.action == "security-unban" or .action == "security-seed") |
      {at_s:.at_s,banned:(.action == "security-seed")}] |
    sort_by(.at_s)' "$case_file")"
  export BENCH_SEMANTIC_MIN="$semantic_min"
  export BENCH_FAULT_AVAILABLE_MIN="$fault_available_min"
  export BENCH_RECOVERY_AVAILABLE_MIN="$recovery_available_min"
  export BENCH_RELOAD_START_S="$reload_start_s"
  export BENCH_PRODUCT_DIR="$current_product_dir"

  printf 'starting product=%s profile=%s ports=%s\n' "$product" "$profile" "${ports[*]}"
  "$current_adapter" preflight >"$current_product_dir/logs/preflight.log" 2>&1 || product_failed=1
  ((product_failed == 0)) || { printf 'FAILED\n' >"$current_product_dir/status.txt"; cleanup_product; return 1; }

  local upstream variable delay index=0
  for upstream in A B C D E; do
    variable="BENCH_UPSTREAM_${upstream}_PORT"
    delay=$((index + 1))
    python3 "$repo/test/api_gw/mock_rich.py" --host 127.0.0.1 --port "${!variable}" \
      --name "bench-${upstream,,}" --seed "$((seed + index))" \
      --delay-p50-ms "$delay" --delay-p99-ms "$((delay + 2))" --max-normal-delay-ms 10 \
      --body-sizes 256 >"$current_product_dir/logs/mock-${upstream,,}.log" 2>&1 &
    mock_pids+=("$!")
    index=$((index + 1))
  done
  index=0
  for upstream in A B C D E; do
    variable="BENCH_UPSTREAM_${upstream}_PORT"
    wait_url "http://127.0.0.1:${!variable}/_soak/health" "${mock_pids[$index]}" || product_failed=1
    index=$((index + 1))
  done
  if ((product_failed == 0)); then direct_calibration >"$current_product_dir/logs/direct.log" 2>&1 || product_failed=1; fi
  if ((product_failed == 0)); then "$current_adapter" prepare >"$current_product_dir/logs/prepare.log" 2>&1 || product_failed=1; fi
  if ((product_failed == 0)); then "$current_adapter" start >"$current_product_dir/logs/start.log" 2>&1 || product_failed=1; fi
  if ((product_failed == 0)); then "$current_adapter" ready >"$current_product_dir/logs/ready.log" 2>&1 || product_failed=1; fi
  if ((product_failed != 0)); then
    printf 'FAILED\n' >"$current_product_dir/status.txt"
    cleanup_product
    return 1
  fi

  refresh_pids
  "$current_adapter" security-seed >"$current_product_dir/logs/security-seed.log" 2>&1 || product_failed=1
  export BENCH_START_EPOCH_MS=0
  BENCH_WS_CLIENTS=0 run_k6 warmup "$warmup_s" warmup || product_failed=1
  for upstream in A B C D E; do
    mock_control "$upstream" '{"error_probability":0,"drop_probability":0,"slow_probability":0,"healthy":true,"clear_pending_errors":true,"reset_stats":true}' || product_failed=1
  done
  snapshot_mocks before
  "$current_adapter" snapshot-before >"$current_product_dir/logs/snapshot-before.log" 2>&1 || product_failed=1
  if ((product_failed != 0)); then
    printf 'FAILED\n' >"$current_product_dir/status.txt"
    cleanup_product
    return 1
  fi

  export BENCH_START_EPOCH_MS="$(( $(date +%s%3N) + 5000 ))"
  printf '%s\n' "$BENCH_START_EPOCH_MS" >"$current_product_dir/formal-start-epoch-ms.txt"
  python3 "$suite_dir/lib/resource_sampler.py" --pid-file "$current_product_dir/pids.current" \
    --cgroup-file "$current_product_dir/cgroup.path" --output "$current_product_dir/resources.csv" \
    --start-epoch-ms "$BENCH_START_EPOCH_MS" --duration-s "$duration_s" &
  sampler_pid=$!
  run_k6 formal "$duration_s" formal &
  k6_pid=$!
  run_events "$BENCH_START_EPOCH_MS"
  if ! wait "$k6_pid"; then product_failed=1; fi
  k6_pid=""
  stop_pid "$sampler_pid" resource-sampler
  sampler_pid=""
  "$current_adapter" snapshot-after >"$current_product_dir/logs/snapshot-after.log" 2>&1 || product_failed=1
  snapshot_mocks after || product_failed=1
  jq -s '
    {
      all_upstreams_health_checked: all(.[]; .health_requests > 0),
      all_upstreams_reachable: all(.[]; .tcp_accepts > 0),
      total_data_requests: (map(.data_requests // .requests // 0) | add),
      total_reused_requests: (map(.reused_requests // 0) | add),
      per_upstream_data_requests: (map(.data_requests // .requests // 0))
    }
    | .passed = (
        .all_upstreams_health_checked
        and .all_upstreams_reachable
        and .total_data_requests > 0
        and .total_reused_requests > 0
      )
  ' "$current_product_dir"/snapshots/after/upstream-{a,b,c,d,e}.json \
    >"$current_product_dir/snapshots/upstream-contract.json" || product_failed=1
  jq -e '.passed == true' "$current_product_dir/snapshots/upstream-contract.json" >/dev/null \
    || product_failed=1
  refresh_pids

  python3 "$suite_dir/lib/summarize.py" --product "$product" \
    --k6-summary "$current_product_dir/raw/formal-summary.json" \
    --resources "$current_product_dir/resources.csv" --events "$current_product_dir/events.jsonl" \
    --duration-s "$duration_s" \
    --output "$current_product_dir/summary.json" || product_failed=1
  if [[ -f "$current_product_dir/raw/k6-metrics.json" ]]; then
    gzip -f "$current_product_dir/raw/k6-metrics.json" || product_failed=1
  fi
  local product_status_file="$current_product_dir/status.txt"
  local finished_product_dir="$current_product_dir"
  cleanup_product
  compress_product_logs "$finished_product_dir" || product_failed=1
  if ((product_failed)); then
    printf 'FAILED\n' >"$product_status_file"
    return 1
  fi
  printf 'PASSED\n' >"$product_status_file"
  return 0
}

capture_metadata
mapfile -t product_order < <(python3 - "$seed" "${requested_products[@]}" <<'PY'
import random, sys
seed = int(sys.argv[1])
products = sys.argv[2:]
random.Random(seed).shuffle(products)
print(*products, sep="\n")
PY
)
printf '%s\n' "${product_order[@]}" >"$run_root/product-order.txt"
printf 'run=%s case=%s serial_order=%s\n' "$run_id" "$case_name" "${product_order[*]}"

for product in "${product_order[@]}"; do
  if run_product "$product"; then
    printf 'product=%s result=PASSED\n' "$product"
  else
    suite_failed=1
    printf 'product=%s result=FAILED\n' "$product"
  fi
done

python3 - "$run_root" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
rows = []
for product in (root / "products").iterdir():
    summary = product / "summary.json"
    rows.append(json.loads(summary.read_text(encoding="utf-8")) if summary.exists() else {
        "product": product.name,
        "status": (product / "status.txt").read_text(encoding="utf-8").strip(),
    })
(root / "summary.json").write_text(json.dumps(rows, indent=2, sort_keys=True) + "\n", encoding="utf-8")
def shown(value):
    return "-" if value is None else value

lines = ["# API Gateway Comparison Run", "", "| Product | Status | HTTP RPS | Normal P99 ms | PSS median KiB | Memory peak bytes |", "|---|---:|---:|---:|---:|---:|"]
for row in sorted(rows, key=lambda item: item["product"]):
    status_path = root / "products" / row["product"] / "status.txt"
    status = status_path.read_text(encoding="utf-8").strip() if status_path.exists() else row.get("status", "UNKNOWN")
    lines.append("| {product} | {status} | {http_rps} | {p99} | {pss} | {memory} |".format(
        product=row["product"], status=status, http_rps=row.get("http_rps", ""),
        p99=shown(row.get("normal_p99_ms")), pss=shown(row.get("pss_median_kb")),
        memory=shown(row.get("memory_peak_bytes"))))
(root / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

finished=1
if ((suite_failed)); then printf 'FAILED\n' >"$run_root/status.txt"; else printf 'PASSED\n' >"$run_root/status.txt"; fi
exit "$suite_failed"
