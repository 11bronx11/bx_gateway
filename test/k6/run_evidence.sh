#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
k6_bin="${K6:-k6}"
out_dir="${K6_EVIDENCE_DIR:-$repo/test/k6/evidence/$(date -u +%Y%m%dT%H%M%SZ)}"
soak_duration="${K6_EVIDENCE_SOAK_DURATION:-1h}"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

[[ -n "${K6_GW_PID:-}" ]] || die "K6_GW_PID is required"
[[ -n "${K6_HUB_PID:-}" ]] || die "K6_HUB_PID is required"
command -v "$k6_bin" >/dev/null 2>&1 || die "k6 not found: $k6_bin"
[[ -r "/proc/$K6_GW_PID/status" ]] || die "gateway PID is not running: $K6_GW_PID"
[[ -r "/proc/$K6_HUB_PID/status" ]] || die "hub PID is not running: $K6_HUB_PID"
[[ "${K6_MOCK_PORTS:-}" == *,* ]] || die "K6_MOCK_PORTS must list at least two mock ports"
[[ -n "${K6_SLOW_API_PATH:-}" ]] || die "K6_SLOW_API_PATH must isolate the controlled slow upstream"
[[ -x "${BANCTL:-$repo/bin/banctl}" ]] || die "banctl not found: ${BANCTL:-$repo/bin/banctl}"
export K6_HUB_PORT="${K6_HUB_PORT:-9091}"

mkdir -p "$out_dir"

snapshot_process() {
  local name="$1"
  local pid="$2"
  local out="$3"
  {
    printf 'captured_at_utc='; date -u +%Y-%m-%dT%H:%M:%SZ
    printf 'pid=%s\n' "$pid"
    awk '/^(Name|State|Threads|VmRSS|VmHWM|FDSize):/ { print }' "/proc/$pid/status"
    printf 'open_fds='
    find "/proc/$pid/fd" -mindepth 1 -maxdepth 1 -print 2>/dev/null | wc -l
  } >"$out/${name}.txt"
}

write_metadata() {
  {
    printf 'generated_at_utc='; date -u +%Y-%m-%dT%H:%M:%SZ
    printf 'git_head='; git -C "$repo" rev-parse HEAD
    printf 'k6_version='; "$k6_bin" version
    printf 'soak_duration=%s\n' "$soak_duration"
    printf '\n[worktree]\n'
    git -C "$repo" status --short
    printf '\n[sha256]\n'
    find "$repo/test/k6" -type f ! -path '*/evidence/*' -print0 \
      | sort -z | xargs -0 sha256sum
    sha256sum "$repo/api_gw/bin/gateway.yml" "$repo/api_gw/bin/hub.yml"
  } >"$out_dir/manifest.txt"
}

run_case() {
  local name="$1"
  local script="$2"
  local stage_dir="$out_dir/$name"
  mkdir -p "$stage_dir/before" "$stage_dir/after"
  snapshot_process gw "$K6_GW_PID" "$stage_dir/before"
  snapshot_process hub "$K6_HUB_PID" "$stage_dir/before"
  local statuses
  if "$k6_bin" run --summary-export "$stage_dir/summary.json" "$script" \
    2>&1 | tee "$stage_dir/k6.log"; then
    statuses=("${PIPESTATUS[@]}")
  else
    statuses=("${PIPESTATUS[@]}")
  fi
  snapshot_process gw "$K6_GW_PID" "$stage_dir/after"
  snapshot_process hub "$K6_HUB_PID" "$stage_dir/after"
  if (( statuses[0] != 0 )); then return "${statuses[0]}"; fi
  return "${statuses[1]}"
}

run_ipban_case() {
  local stage_dir="$out_dir/09_ipban_preban"
  mkdir -p "$stage_dir/before" "$stage_dir/after"
  snapshot_process gw "$K6_GW_PID" "$stage_dir/before"
  snapshot_process hub "$K6_HUB_PID" "$stage_dir/before"
  local statuses
  if K6_SUMMARY_DIR="$stage_dir" \
    K6_IPBAN_TEST_IP="${K6_IPBAN_PREBAN_IP:-198.51.100.243}" \
    bash "$repo/test/k6/run_ipban.sh" 2>&1 | tee "$stage_dir/k6.log"; then
    statuses=("${PIPESTATUS[@]}")
  else
    statuses=("${PIPESTATUS[@]}")
  fi
  snapshot_process gw "$K6_GW_PID" "$stage_dir/after"
  snapshot_process hub "$K6_HUB_PID" "$stage_dir/after"
  if (( statuses[0] != 0 )); then return "${statuses[0]}"; fi
  return "${statuses[1]}"
}

cleanup_risk_ips() {
  local log="$1"
  shift
  local banctl="${BANCTL:-$repo/bin/banctl}"
  local admin_sock="${HUB_ADMIN_SOCK:-/tmp/bronx_ip_admin.sock}"
  local ip status=0
  : >"$log"
  sleep 1 # 等 Reporter 队列把本阶段的 risk 送进 Hub，再按真实 rule id 删除。
  for ip in "$@"; do
    "$banctl" --sock "$admin_sock" del "waf:$ip" >>"$log" 2>&1 || status=1
    "$banctl" --sock "$admin_sock" del "rate:$ip" >>"$log" 2>&1 || status=1
  done
  return "$status"
}

run_case_with_risk_cleanup() {
  local name="$1"
  local script="$2"
  shift 2
  local status cleanup_status
  if run_case "$name" "$script"; then
    status=0
  else
    status=$?
  fi
  if cleanup_risk_ips "$out_dir/$name/cleanup.log" "$@"; then
    cleanup_status=0
  else
    cleanup_status=$?
  fi
  if (( status != 0 )); then return "$status"; fi
  return "$cleanup_status"
}

run_auto_ipban_case() {
  local test_ip="${K6_IPBAN_AUTO_IP:-198.51.100.241}"
  local banctl="${BANCTL:-$repo/bin/banctl}"
  local admin_sock="${HUB_ADMIN_SOCK:-/tmp/bronx_ip_admin.sock}"
  local status cleanup_status

  if K6_AUTO_BAN=1 \
    K6_IPBAN_TEST_IP="$test_ip" \
    K6_IPBAN_ALLOW_IP="${K6_IPBAN_ALLOW_IP:-198.51.100.242}" \
    run_case 09_ipban_autoban "$repo/test/k6/scenarios/09_ipban.js"; then
    status=0
  else
    status=$?
  fi
  if "$banctl" --sock "$admin_sock" del "waf:$test_ip" >"$out_dir/09_ipban_autoban/cleanup.log" 2>&1; then
    cleanup_status=0
  else
    cleanup_status=$?
  fi

  if (( status != 0 )); then return "$status"; fi
  return "$cleanup_status"
}

write_metadata

waf_seed_ip="${K6_WAF_PROBE_IP:-198.51.100.230}"
rate_seed_ip="${K6_RATE_PROBE_IP:-198.51.100.240}"
[[ "$waf_seed_ip" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || die "K6_WAF_PROBE_IP must be IPv4"
[[ "$rate_seed_ip" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || die "K6_RATE_PROBE_IP must be IPv4"
IFS=. read -r waf_a waf_b waf_c _ <<<"$waf_seed_ip"
IFS=. read -r rate_a rate_b rate_c _ <<<"$rate_seed_ip"
waf_ips=()
rate_soak_ips=()
for ((last = 1; last <= 13; ++last)); do waf_ips+=("$waf_a.$waf_b.$waf_c.$last"); done
for ((last = 101; last <= 200; ++last)); do rate_soak_ips+=("$rate_a.$rate_b.$rate_c.$last"); done

for script in \
  "$repo/test/k6/smoke.js" \
  "$repo/test/k6/scenarios/01_auth.js" \
  "$repo/test/k6/scenarios/02_cors_headers.js" \
  "$repo/test/k6/scenarios/03_rate_limit.js" \
  "$repo/test/k6/scenarios/04_waf.js" \
  "$repo/test/k6/scenarios/05_websocket.js" \
  "$repo/test/k6/scenarios/06_routing.js" \
  "$repo/test/k6/scenarios/07_upstream_gov.js" \
  "$repo/test/k6/scenarios/08_lb_distribution.js" \
  "$repo/test/k6/scenarios/10_reload.js" \
  "$repo/test/k6/scenarios/11_body.js" \
  "$repo/test/k6/scenarios/12_slow_attack.js" \
  "$repo/test/k6/scenarios/13_observability.js" \
  "$repo/test/k6/scenarios/14_slowloris.js" \
  "$repo/test/k6/scenarios/15_xff_trusted.js"; do
  name="$(basename "$script" .js)"
  case "$name" in
    03_rate_limit)
      run_case_with_risk_cleanup "$name" "$script" \
        "${K6_RATE_BURST_IP:-198.51.100.201}" \
        "${K6_RATE_REFILL_IP:-198.51.100.202}" \
        "${K6_RATE_STEADY_IP:-198.51.100.203}" \
        "${K6_RATE_ROUTE_IP:-198.51.100.204}"
      ;;
    04_waf)
      run_case_with_risk_cleanup "$name" "$script" "${waf_ips[@]}"
      ;;
    15_xff_trusted)
      run_case_with_risk_cleanup "$name" "$script" \
        203.0.113.10 203.0.113.20 192.0.2.99
      ;;
    *) run_case "$name" "$script" ;;
  esac
done

run_auto_ipban_case
run_ipban_case

SOAK_DURATION="$soak_duration" run_case_with_risk_cleanup soak "$repo/test/k6/soak.js" \
  "${waf_ips[0]}" "${rate_soak_ips[@]}"
printf 'evidence_dir=%s\n' "$out_dir"
