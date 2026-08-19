#!/usr/bin/env bash
set -Eeuo pipefail

suite_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$suite_dir/../../.." && pwd)"
repro_dir="$repo/benchmark/api_gateway_comparison/repro"
case_name="formal"
products_override=""
results_root="$suite_dir/results"
seed="${BENCH_SEED:-20260818}"
target_cpus="${BENCH_TARGET_CPUS:-1,2}"
load_cpu="${BENCH_LOAD_CPU:-0}"
upstream_cpu="${BENCH_UPSTREAM_CPU:-3}"
save_raw="${BENCH_SAVE_RAW:-0}"

usage() {
  cat <<'EOF'
Usage: ./run.sh [options]

Options:
  --case NAME          cases/NAME.json (default: formal)
  --products CSV       Product subset; execution remains serial
  --results-root PATH  Evidence root (default: ./results)
  --seed INTEGER       Deterministic order seed
  --target-cpus LIST   Two CPUs for the tested gateway (default: 1,2)
  --load-cpu CPU       CPU for k6/Vegeta (default: 0)
  --upstream-cpu CPU   CPU shared by both healthy upstreams (default: 3)
  --save-raw           Retain gzip-compressed k6 sample stream
  -h, --help           Show this help

Examples:
  ./run.sh --case smoke
  ./run.sh --case formal
  ./run.sh --case formal --products bronx,spring
EOF
}

while (($#)); do
  case "$1" in
    --case) case_name="${2:?missing --case value}"; shift 2 ;;
    --products) products_override="${2:?missing --products value}"; shift 2 ;;
    --results-root) results_root="${2:?missing --results-root value}"; shift 2 ;;
    --seed) seed="${2:?missing --seed value}"; shift 2 ;;
    --target-cpus) target_cpus="${2:?missing --target-cpus value}"; shift 2 ;;
    --load-cpu) load_cpu="${2:?missing --load-cpu value}"; shift 2 ;;
    --upstream-cpu) upstream_cpu="${2:?missing --upstream-cpu value}"; shift 2 ;;
    --save-raw) save_raw=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

case_file="$suite_dir/cases/$case_name.json"
[[ -f "$case_file" ]] || { printf 'case not found: %s\n' "$case_file" >&2; exit 2; }
for command in python3 jq curl k6 vegeta taskset sha256sum gzip docker; do
  command -v "$command" >/dev/null 2>&1 || { printf 'missing command: %s\n' "$command" >&2; exit 2; }
done
[[ "$seed" =~ ^[0-9]+$ ]] || { printf 'seed must be an integer\n' >&2; exit 2; }
[[ "$save_raw" == 0 || "$save_raw" == 1 ]] || { printf 'BENCH_SAVE_RAW must be 0 or 1\n' >&2; exit 2; }
python3 - "$target_cpus" "$load_cpu" "$upstream_cpu" <<'PY'
import sys
target = {int(value) for value in sys.argv[1].split(",")}
load, upstream = int(sys.argv[2]), int(sys.argv[3])
if len(target) != 2:
    raise SystemExit("--target-cpus must name exactly two distinct CPUs")
if target & {load, upstream} or load == upstream:
    raise SystemExit("gateway, load generator and upstream CPU sets must not overlap")
PY
taskset -c "$target_cpus" true
taskset -c "$load_cpu" true
taskset -c "$upstream_cpu" true

python3 - "$case_file" <<'PY'
import json, pathlib, sys

case = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
positive_ints = ("warmup_s", "stage_seconds", "repeats", "rate_limit_per_sec")
for name in positive_ints:
    value = case.get(name)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise SystemExit(f"{name} must be a positive integer")
rates = case.get("stage_rps")
if not isinstance(rates, list) or not rates:
    raise SystemExit("stage_rps must be a non-empty list")
if any(not isinstance(value, int) or isinstance(value, bool) or value <= 0 for value in rates):
    raise SystemExit("stage_rps values must be positive integers")
if any(current <= previous for previous, current in zip(rates, rates[1:])):
    raise SystemExit("stage_rps must be strictly increasing")
for name in ("semantic_min", "delivery_min", "direct_success_min", "direct_delivery_min"):
    value = case.get(name)
    if not isinstance(value, (int, float)) or isinstance(value, bool) or not 0 < value <= 1:
        raise SystemExit(f"{name} must be in (0, 1]")
value = case.get("dropped_max_ratio")
if not isinstance(value, (int, float)) or isinstance(value, bool) or not 0 <= value <= 1:
    raise SystemExit("dropped_max_ratio must be in [0, 1]")
for name in ("p99_max_ms", "direct_calibration_seconds"):
    value = case.get(name)
    if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
        raise SystemExit(f"{name} must be greater than zero")
PY

warmup_s="$(jq -er '.warmup_s | numbers' "$case_file")"
stage_seconds="$(jq -er '.stage_seconds | numbers' "$case_file")"
stage_rates="$(jq -c '.stage_rps' "$case_file")"
mapfile -t stage_rps_values < <(jq -er '.stage_rps[] | numbers' "$case_file")
stage_count="${#stage_rps_values[@]}"
repeats="$(jq -er '.repeats | numbers' "$case_file")"
semantic_min="$(jq -er '.semantic_min | numbers' "$case_file")"
delivery_min="$(jq -er '.delivery_min | numbers' "$case_file")"
dropped_max_ratio="$(jq -er '.dropped_max_ratio | numbers' "$case_file")"
p99_max_ms="$(jq -er '.p99_max_ms | numbers' "$case_file")"
direct_success_min="$(jq -er '.direct_success_min | numbers' "$case_file")"
direct_delivery_min="$(jq -er '.direct_delivery_min | numbers' "$case_file")"
direct_calibration_seconds="$(jq -er '.direct_calibration_seconds | numbers' "$case_file")"
rate_limit="$(jq -er '.rate_limit_per_sec | numbers' "$case_file")"

if [[ -n "$products_override" ]]; then
  IFS=, read -r -a products <<<"$products_override"
else
  mapfile -t products < <(jq -r '.products[]' "$case_file")
fi
((${#products[@]} > 0)) || { printf 'no products selected\n' >&2; exit 2; }
for product in "${products[@]}"; do
  case "$product" in bronx|spring|kong-dbless|kong-postgres) ;; *) printf 'unknown product: %s\n' "$product" >&2; exit 2 ;; esac
done

run_id="$(date -u +%Y%m%dT%H%M%SZ)-$(printf '%04x' "$((RANDOM & 65535))")"
run_root="$(python3 - "$results_root" "$run_id" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1]).expanduser().resolve() / sys.argv[2]
path.mkdir(parents=True, exist_ok=False)
print(path)
PY
)"
mkdir -p "$run_root/host" "$run_root/source"
products_json="$(printf '%s\n' "${products[@]}" | jq -R . | jq -s .)"
jq --argjson products "$products_json" '.products = $products' "$case_file" >"$run_root/case.json"
case_file="$run_root/case.json"
exec > >(tee -a "$run_root/run.log") 2>&1

current_adapter=""
current_product_dir=""
measurement_dir=""
k6_pid=""
sampler_pid=""
mock_pids=()
finished=0
suite_failed=0

pid_running() {
  [[ -n "${1:-}" ]] && kill -0 "$1" 2>/dev/null
}

stop_pid() {
  local pid="${1:-}" label="${2:-process}"
  [[ -n "$pid" ]] || return 0
  if pid_running "$pid"; then
    kill -TERM "$pid" 2>/dev/null || true
    for _ in {1..30}; do pid_running "$pid" || break; sleep 0.1; done
    if pid_running "$pid"; then
      printf 'forcing stop of %s pid=%s\n' "$label" "$pid" >&2
      kill -KILL "$pid" 2>/dev/null || true
    fi
  fi
  wait "$pid" 2>/dev/null || true
}

cleanup_product() {
  stop_pid "$k6_pid" k6
  stop_pid "$sampler_pid" resource-sampler
  k6_pid=""; sampler_pid=""
  if [[ -n "$current_adapter" && -n "$current_product_dir" ]]; then
    "$current_adapter" stop >>"$current_product_dir/logs/adapter-stop.log" 2>&1 || true
  fi
  local index=0
  for pid in "${mock_pids[@]}"; do stop_pid "$pid" "mock-$index"; index=$((index + 1)); done
  mock_pids=()
  current_adapter=""; current_product_dir=""; measurement_dir=""
}

finalize() {
  local status=$?
  trap - EXIT INT TERM
  cleanup_product || true
  if (( !finished )); then printf 'ABORTED\n' >"$run_root/status.txt"; fi
  printf 'evidence_dir=%s\n' "$run_root"
  (
    cd "$run_root"
    /usr/bin/find . -type f ! -name manifest.sha256 -print0 | sort -z \
      | xargs -0 sha256sum >manifest.sha256
  ) || status=1
  ((status == 0)) || exit "$status"
  exit "$suite_failed"
}
trap finalize EXIT
trap 'exit 130' INT TERM

capture_metadata() {
  local source_file destination
  local source_files=(
    benchmark/api_gateway_comparison/capacity_2c2g/run.sh
    benchmark/api_gateway_comparison/capacity_2c2g/README.md
    benchmark/api_gateway_comparison/capacity_2c2g/cases/formal.json
    benchmark/api_gateway_comparison/capacity_2c2g/cases/smoke.json
    benchmark/api_gateway_comparison/capacity_2c2g/cases/smoke-3x.json
    benchmark/api_gateway_comparison/capacity_2c2g/cases/smoke-stop.json
    benchmark/api_gateway_comparison/capacity_2c2g/load/healthy.js
    benchmark/api_gateway_comparison/capacity_2c2g/lib/summarize.py
    benchmark/api_gateway_comparison/capacity_2c2g/lib/aggregate.py
    benchmark/api_gateway_comparison/repro/lib/resource_sampler.py
    benchmark/api_gateway_comparison/repro/adapters/bronx.sh
    benchmark/api_gateway_comparison/repro/adapters/spring.sh
    benchmark/api_gateway_comparison/repro/adapters/kong-dbless.sh
    benchmark/api_gateway_comparison/repro/adapters/kong-postgres.sh
    benchmark/api_gateway_comparison/repro/products/kong/render.py
    benchmark/api_gateway_comparison/repro/products/kong/plugins/bench-security/handler.lua
    benchmark/api_gateway_comparison/repro/products/kong/plugins/bench-security/schema.lua
    benchmark/api_gateway_comparison/repro/products/spring/pom.xml
    benchmark/api_gateway_comparison/repro/products/spring/application.yml.in
    benchmark/api_gateway_comparison/repro/products/spring/src/main/java/io/bronx/benchmark/GatewayApplication.java
    test/api_gw/mock_rich.py
  )
  {
    printf 'run_id=%s\n' "$run_id"
    printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'case=%s\n' "$case_name"
    printf 'repeats=%s\n' "$repeats"
    printf 'stage_seconds=%s\n' "$stage_seconds"
    printf 'stage_rps=%s\n' "$stage_rates"
    printf 'dropped_max_ratio=%s\n' "$dropped_max_ratio"
    printf 'p99_max_ms=%s\n' "$p99_max_ms"
    printf 'target_cpus=%s\n' "$target_cpus"
    printf 'load_cpu=%s\n' "$load_cpu"
    printf 'upstream_cpu=%s\n' "$upstream_cpu"
    printf 'cpu_quota_percent=200\n'
    printf 'memory_bytes=2147483648\n'
    printf 'upstream_count=2\n'
    printf 'gateway_workers=2\n'
    printf 'rate_limit_per_sec=%s\n' "$rate_limit"
    printf 'git_head=%s\n' "$(git -C "$repo" rev-parse HEAD)"
    printf 'k6=%s\n' "$(k6 version | head -1)"
    printf 'vegeta=%s\n' "$(vegeta -version 2>&1 | head -1)"
    printf 'docker=%s\n' "$(docker version --format '{{.Server.Version}}' 2>/dev/null || printf unavailable)"
  } >"$run_root/metadata.env"
  git -C "$repo" status --short >"$run_root/host/git-status.txt"
  git -C "$repo" diff --binary >"$run_root/host/worktree.patch"
  uname -a >"$run_root/host/uname.txt"
  lscpu >"$run_root/host/lscpu.txt" 2>&1 || true
  cat /proc/meminfo >"$run_root/host/meminfo.txt"
  for source_file in "${source_files[@]}"; do
    [[ -f "$repo/$source_file" ]] || continue
    destination="$run_root/source/$source_file"
    mkdir -p "$(dirname "$destination")"
    cp "$repo/$source_file" "$destination"
  done
  (cd "$run_root/source" && /usr/bin/find . -type f -print0 | sort -z | xargs -0 sha256sum >../source.sha256)
}

allocate_ports() {
  python3 - "$1" <<'PY'
import socket, sys
sockets=[]
try:
    for _ in range(int(sys.argv[1])):
        value=socket.socket(); value.bind(("127.0.0.1",0)); sockets.append(value)
        print(value.getsockname()[1])
finally:
    for value in sockets: value.close()
PY
}

wait_url() {
  local url="$1" pid="$2"
  for _ in {1..150}; do
    pid_running "$pid" || return 1
    curl -fsS --max-time 1 "$url" >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  return 1
}

refresh_pids() {
  "$current_adapter" pids >"$current_product_dir/pids.current.tmp"
  mv "$current_product_dir/pids.current.tmp" "$current_product_dir/pids.current"
}

snapshot_mocks() {
  local label="$1" output_root="${2:-$current_product_dir}" name variable
  mkdir -p "$output_root/snapshots/$label"
  for name in A B; do
    variable="BENCH_UPSTREAM_${name}_PORT"
    curl -fsS --max-time 3 "http://127.0.0.1:${!variable}/_soak/stats" \
      >"$output_root/snapshots/$label/upstream-${name,,}.json"
  done
}

mock_control() {
  local name="$1" variable="BENCH_UPSTREAM_${1}_PORT"
  curl -fsS --max-time 3 -X POST -H 'Content-Type: application/json' \
    -d '{"error_probability":0,"drop_probability":0,"slow_probability":0,"healthy":true,"clear_pending_errors":true,"reset_stats":true}' \
    "http://127.0.0.1:${!variable}/_soak/control" >/dev/null
}

direct_calibration() {
  local target_rps="$1" output_root="$2"
  local rate=$(((target_rps + 1) / 2)) name variable pid
  local calibration_pids=()
  ((rate > 0)) || rate=1
  mkdir -p "$output_root"
  for name in A B; do
    variable="BENCH_UPSTREAM_${name}_PORT"
    (
      printf 'GET http://127.0.0.1:%s/api/items\n' "${!variable}" \
        | taskset -c "$load_cpu" vegeta attack -rate "$rate" \
          -duration "${direct_calibration_seconds}s" -timeout 2s \
          >"$output_root/direct-${name,,}.bin"
    ) &
    calibration_pids+=("$!")
  done
  for pid in "${calibration_pids[@]}"; do wait "$pid"; done
  for name in A B; do
    vegeta report -type=json "$output_root/direct-${name,,}.bin" \
      >"$output_root/direct-${name,,}-summary.json"
  done
  jq -n \
    --argjson target_rps "$target_rps" --argjson per_upstream_rps "$rate" \
    --argjson duration_s "$direct_calibration_seconds" \
    --argjson success_min "$direct_success_min" --argjson delivery_min "$direct_delivery_min" \
    --slurpfile a "$output_root/direct-a-summary.json" \
    --slurpfile b "$output_root/direct-b-summary.json" '
      def endpoint($name; $value): {
        name:$name,
        requests:$value.requests,
        actual_rate:$value.rate,
        success:$value.success,
        p99_ms:($value.latencies["99th"] / 1000000),
        passed:(($value.success >= $success_min) and ($value.rate >= ($per_upstream_rps * $delivery_min)))
      };
      {
        target_rps:$target_rps,
        requested_per_upstream_rps:$per_upstream_rps,
        duration_s:$duration_s,
        endpoints:[endpoint("A"; $a[0]), endpoint("B"; $b[0])]
      }
      | .passed=all(.endpoints[]; .passed)
      | .failure_owner=(if .passed then "gateway_path_or_k6" else "load_generator_or_upstream" end)
    ' >"$output_root/summary.json"
}

run_k6() {
  local mode="$1" prefix="$2" output_root="${3:-$current_product_dir}"
  local target_rps="${4:-${stage_rps_values[0]}}" stage_index="${5:-1}" raw=()
  if [[ "$save_raw" == 1 && "$mode" == formal ]]; then
    raw=(--out "json=$output_root/raw/k6-metrics.json")
  fi
  BENCH_MODE="$mode" BENCH_GATEWAY_URL="$BENCH_GATEWAY_URL" \
  BENCH_JWT_SECRET="$BENCH_JWT_SECRET" BENCH_JWT_ISSUER="$BENCH_JWT_ISSUER" \
  BENCH_STAGE_RATES="$stage_rates" BENCH_STAGE_SECONDS="$stage_seconds" \
  BENCH_TARGET_RPS="$target_rps" BENCH_STAGE_INDEX="$stage_index" \
  BENCH_WARMUP_S="$warmup_s" BENCH_START_EPOCH_MS="${BENCH_START_EPOCH_MS:-0}" \
  taskset -c "$load_cpu" k6 run --summary-export "$output_root/raw/$prefix-summary.json" \
    "${raw[@]}" "$suite_dir/load/healthy.js" >"$output_root/logs/$prefix-k6.log" 2>&1
}

run_one() {
  local product="$1" product_failed=0 repeat repeat_failed stage_failed
  local name variable index=0 stage_index target_rps stage_dir stage_stable
  current_product_dir="$run_root/products/$product"
  current_adapter="$repro_dir/adapters/$product.sh"
  mkdir -p "$current_product_dir"/{configs,logs,raw,runtime,snapshots}
  printf 'RUNNING\n' >"$current_product_dir/status.txt"

  mapfile -t ports < <(allocate_ports 8)
  export BENCH_GATEWAY_PORT="${ports[0]}" BENCH_ADMIN_PORT="${ports[1]}" BENCH_HUB_PORT="${ports[2]}"
  export BENCH_UPSTREAM_A_PORT="${ports[3]}" BENCH_UPSTREAM_B_PORT="${ports[4]}"
  export BENCH_UPSTREAM_C_PORT="${ports[5]}" BENCH_UPSTREAM_D_PORT="${ports[6]}" BENCH_UPSTREAM_E_PORT="${ports[7]}"
  export BENCH_GATEWAY_URL="http://127.0.0.1:$BENCH_GATEWAY_PORT"
  export BENCH_JWT_SECRET="capacity-secret-20260818-32-bytes-minimum" BENCH_JWT_ISSUER="capacity-issuer"
  export BENCH_BANNED_IP="198.18.40.40" BENCH_RATE_IP="198.18.50.50"
  export BENCH_RATE_CAPACITY="$rate_limit" BENCH_RATE_REFILL="$rate_limit"
  export BENCH_TARGET_CPU="$target_cpus" BENCH_CPU_QUOTA_PERCENT=200
  export BENCH_MEMORY_BYTES=2147483648 BENCH_UPSTREAM_COUNT=2 BENCH_GATEWAY_WORKERS=2
  export BENCH_CAPACITY_MODE=1
  export BENCH_KONG_CPUS=2 BENCH_PG_CPUS=0.25
  export BENCH_KONG_MEMORY_BYTES=1879048192 BENCH_PG_MEMORY_BYTES=268435456
  [[ "$product" == kong-postgres ]] && export BENCH_KONG_CPUS=1.75
  export BENCH_SPRING_XMX_MB=1280 BENCH_SPRING_DIRECT_MB=384
  export BENCH_UNIT_TOKEN="${run_id:0:16}-${product//[^a-zA-Z0-9]/-}"
  export BENCH_PRODUCT="$product" BENCH_PRODUCT_DIR="$current_product_dir"

  printf 'product=%s ports=%s\n' "$product" "${ports[*]}"
  "$current_adapter" preflight >"$current_product_dir/logs/preflight.log" 2>&1 || product_failed=1
  if ((product_failed)); then printf 'FAILED\n' >"$current_product_dir/status.txt"; cleanup_product; return 1; fi

  for name in A B; do
    variable="BENCH_UPSTREAM_${name}_PORT"
    taskset -c "$upstream_cpu" python3 "$repo/test/api_gw/mock_rich.py" \
      --host 127.0.0.1 --port "${!variable}" --name "bench-${name,,}" \
      --seed "$((seed + index))" --delay-p50-ms 1 --delay-p99-ms 2 \
      --max-normal-delay-ms 3 --body-sizes 256 \
      >"$current_product_dir/logs/mock-${name,,}.log" 2>&1 &
    mock_pids+=("$!")
    index=$((index + 1))
  done
  index=0
  for name in A B; do
    variable="BENCH_UPSTREAM_${name}_PORT"
    wait_url "http://127.0.0.1:${!variable}/_soak/health" "${mock_pids[$index]}" || product_failed=1
    index=$((index + 1))
  done
  ((product_failed == 0)) && "$current_adapter" prepare >"$current_product_dir/logs/prepare.log" 2>&1 || product_failed=1
  ((product_failed == 0)) && "$current_adapter" start >"$current_product_dir/logs/start.log" 2>&1 || product_failed=1
  ((product_failed == 0)) && "$current_adapter" ready >"$current_product_dir/logs/ready.log" 2>&1 || product_failed=1
  if ((product_failed)); then printf 'FAILED\n' >"$current_product_dir/status.txt"; cleanup_product; return 1; fi

  refresh_pids || product_failed=1
  export BENCH_START_EPOCH_MS=0
  run_k6 warmup warmup "$current_product_dir" || product_failed=1
  "$current_adapter" snapshot-before >"$current_product_dir/logs/snapshot-before.log" 2>&1 || product_failed=1
  if ((product_failed)); then printf 'FAILED\n' >"$current_product_dir/status.txt"; cleanup_product; return 1; fi

  for ((repeat = 1; repeat <= repeats; ++repeat)); do
    repeat_failed=0
    measurement_dir="$run_root/round-$(printf '%02d' "$repeat")/products/$product"
    mkdir -p "$measurement_dir"/stages
    printf 'RUNNING\n' >"$measurement_dir/status.txt"
    for ((stage_index = 1; stage_index <= stage_count; ++stage_index)); do
      stage_failed=0
      target_rps="${stage_rps_values[$((stage_index - 1))]}"
      stage_dir="$measurement_dir/stages/$target_rps"
      mkdir -p "$stage_dir"/{logs,raw,snapshots}
      printf 'RUNNING\n' >"$stage_dir/status.txt"
      for name in A B; do mock_control "$name" || stage_failed=1; done
      snapshot_mocks before "$stage_dir" || stage_failed=1
      refresh_pids || stage_failed=1
      export BENCH_START_EPOCH_MS=0
      python3 "$repro_dir/lib/resource_sampler.py" --pid-file "$current_product_dir/pids.current" \
        --cgroup-file "$current_product_dir/cgroup.path" --output "$stage_dir/resources.csv" &
      sampler_pid=$!
      run_k6 formal formal "$stage_dir" "$target_rps" "$stage_index" &
      k6_pid=$!
      if ! wait "$k6_pid"; then stage_failed=1; fi
      k6_pid=""
      stop_pid "$sampler_pid" resource-sampler
      sampler_pid=""
      snapshot_mocks after "$stage_dir" || stage_failed=1
      jq -s '
        {
          all_healthy:all(.[]; .health_requests>0),
          all_used:all(.[]; (.data_requests//.requests//0)>0),
          all_reused:all(.[]; (.reused_requests//0)>0)
        }
        | .passed=(.all_healthy and .all_used and .all_reused)
      ' "$stage_dir"/snapshots/after/upstream-{a,b}.json \
        >"$stage_dir/snapshots/upstream-contract.json" || stage_failed=1
      python3 "$suite_dir/lib/summarize.py" stage --case "$case_file" \
        --stage-index "$stage_index" --target-rps "$target_rps" \
        --k6-summary "$stage_dir/raw/formal-summary.json" --resources "$stage_dir/resources.csv" \
        --snapshot-dir "$stage_dir/snapshots/after" \
        --upstream-contract "$stage_dir/snapshots/upstream-contract.json" \
        --output "$stage_dir/summary.json" || stage_failed=1
      [[ ! -f "$stage_dir/raw/k6-metrics.json" ]] || gzip -f "$stage_dir/raw/k6-metrics.json" || stage_failed=1
      if ((stage_failed)); then
        repeat_failed=1
        printf 'FAILED\n' >"$stage_dir/status.txt"
        break
      fi
      printf 'COMPLETED\n' >"$stage_dir/status.txt"
      stage_stable="$(jq -er '.stable' "$stage_dir/summary.json")"
      printf 'product=%s round=%s target_rps=%s stable=%s\n' \
        "$product" "$repeat" "$target_rps" "$stage_stable"
      if [[ "$stage_stable" != true ]]; then
        direct_calibration "$target_rps" "$measurement_dir/calibration" \
          >"$stage_dir/logs/direct-calibration.log" 2>&1 || repeat_failed=1
        break
      fi
    done
    python3 "$suite_dir/lib/summarize.py" round --product "$product" --round "$repeat" \
      --case "$case_file" --round-dir "$measurement_dir" \
      --output "$measurement_dir/summary.json" || repeat_failed=1
    if ((repeat_failed)); then
      product_failed=1
      printf 'FAILED\n' >"$measurement_dir/status.txt"
    else
      printf 'COMPLETED\n' >"$measurement_dir/status.txt"
    fi
  done
  "$current_adapter" snapshot-after >"$current_product_dir/logs/snapshot-after.log" 2>&1 || product_failed=1

  local status_file="$current_product_dir/status.txt"
  cleanup_product
  if ((product_failed)); then printf 'FAILED\n' >"$status_file"; return 1; fi
  printf 'COMPLETED\n' >"$status_file"
  return 0
}

capture_metadata
mkdir -p "$run_root/products"
mapfile -t order < <(python3 - "$seed" "${products[@]}" <<'PY'
import random, sys
values=sys.argv[2:]
random.Random(int(sys.argv[1])).shuffle(values)
print(*values, sep="\n")
PY
)
printf '%s\n' "${order[@]}" >"$run_root/product-order.txt"
for product in "${order[@]}"; do
  if run_one "$product"; then
    printf 'product=%s result=COMPLETED\n' "$product"
  else
    suite_failed=1
    printf 'product=%s result=FAILED\n' "$product"
  fi
done

python3 "$suite_dir/lib/aggregate.py" --root "$run_root" --case "$case_file" \
  --output "$run_root/summary.json" --report "$run_root/report.md" || suite_failed=1
finished=1
if ((suite_failed)); then printf 'FAILED\n' >"$run_root/status.txt"; else printf 'COMPLETED\n' >"$run_root/status.txt"; fi
exit "$suite_failed"
