#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"

duration="${DURATION_SECONDS:-12}"
warmup_duration="${WARMUP_SECONDS:-3}"
rates="${RATES:-10000 20000 30000 40000 50000 60000}"
bronx_rates="${BRONX_RATES:-$rates}"
nginx_rates="${NGINX_RATES:-$rates}"
rounds="${ROUNDS:-1}"
proxies="${PROXIES:-bronx nginx}"
workers="${VEGETA_WORKERS:-256}"
max_workers="${VEGETA_MAX_WORKERS:-1024}"
connections="${VEGETA_CONNECTIONS:-1024}"
load_cpus="${LOAD_CPUS:-0-1}"
proxy_cpu="${PROXY_CPU:-2}"
upstream_cpu="${UPSTREAM_CPU:-3}"
io_workers="${IO_WORKERS:-1}"
source_mode="${SOURCE_MODE:-head}"
upstream_port="${UPSTREAM_PORT:-18080}"
nginx_port="${NGINX_PORT:-18081}"
bronx_port="${BRONX_PORT:-18082}"
admin_port="${ADMIN_PORT:-18083}"
direct_rate="${DIRECT_RATE:-80000}"
run_direct="${RUN_DIRECT:-1}"
keep_raw="${KEEP_RAW:-0}"
keep_work="${KEEP_WORK:-0}"
run_id="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
out_dir="${OUT_DIR:-$script_dir/runs/$run_id}"

case "$out_dir" in
    /*) ;;
    *) out_dir="$repo_root/$out_dir" ;;
esac

usage() {
    cat <<'EOF'
Usage: benchmark/gateway_vs_nginx/run.sh

Environment overrides:
  RATES="10000 20000 ..."  ROUNDS=1  PROXIES="bronx nginx"
  BRONX_RATES=<rates>        NGINX_RATES=<rates>  IO_WORKERS=1
  SOURCE_MODE=head|worktree
  DURATION_SECONDS=12       WARMUP_SECONDS=3
  LOAD_CPUS=0-1             PROXY_CPU=2       UPSTREAM_CPU=3
  DIRECT_RATE=80000         RUN_DIRECT=1
  OUT_DIR=<path>            KEEP_RAW=0         KEEP_WORK=0

The script exports Git HEAD (or the tracked worktree) to /tmp, builds
RelWithDebInfo there, and never edits the checkout's api_gw/bin YAML files.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

require() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing required command: $1" >&2
        exit 1
    }
}

for tool in cmake curl git jq lscpu nginx pidstat sed sha256sum ss tar taskset vegeta; do
    require "$tool"
done

if (( $(nproc) < 4 )); then
    echo "this profile needs at least four online CPUs" >&2
    exit 1
fi
if [[ ! "$duration" =~ ^[1-9][0-9]*$ || ! "$warmup_duration" =~ ^[1-9][0-9]*$ ]]; then
    echo "DURATION_SECONDS and WARMUP_SECONDS must be positive integers" >&2
    exit 1
fi
if [[ ! "$rounds" =~ ^[1-9][0-9]*$ ]]; then
    echo "ROUNDS must be a positive integer" >&2
    exit 1
fi
if [[ "$source_mode" != "head" && "$source_mode" != "worktree" ]]; then
    echo "SOURCE_MODE must be head or worktree" >&2
    exit 1
fi
if [[ -e "$out_dir" && -n "$(find "$out_dir" -mindepth 1 -maxdepth 1 2>/dev/null)" ]]; then
    echo "refusing to overwrite non-empty OUT_DIR: $out_dir" >&2
    exit 1
fi

for port in "$upstream_port" "$nginx_port" "$bronx_port" "$admin_port"; do
    if ss -H -ltn "sport = :$port" | grep -q .; then
        echo "TCP port already in use: $port" >&2
        exit 1
    fi
done

mkdir -p "$out_dir/configs" "$out_dir/results"
work_dir="$(mktemp -d /tmp/bronx-gateway-bench.XXXXXX)"
src_dir="$work_dir/src"
build_dir="$work_dir/build"
upstream_prefix="$work_dir/nginx-upstream"
nginx_prefix="$work_dir/nginx-proxy"
bronx_pid=""
nginx_master_pid=""
nginx_worker_pid=""
upstream_master_pid=""
upstream_worker_pid=""

stop_pid() {
    local pid="$1"
    local signal="${2:-TERM}"
    [[ -n "$pid" ]] || return 0
    kill -0 "$pid" 2>/dev/null || return 0
    kill -s "$signal" "$pid" 2>/dev/null || true
    for _ in $(seq 1 50); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
}

cleanup() {
    local rc=$?
    stop_pid "$bronx_pid" TERM
    stop_pid "$nginx_master_pid" QUIT
    stop_pid "$upstream_master_pid" QUIT
    if [[ "$keep_work" == "1" ]]; then
        echo "work directory kept: $work_dir"
    else
        rm -rf -- "$work_dir"
    fi
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

log() {
    printf '%s %s\n' "$(date -u +%FT%TZ)" "$*" | tee -a "$out_dir/commands.log"
}

render() {
    local input="$1"
    local output="$2"
    sed -e "s/@UPSTREAM_PORT@/$upstream_port/g" \
        -e "s/@NGINX_PORT@/$nginx_port/g" \
        -e "s/@BRONX_PORT@/$bronx_port/g" \
        -e "s/@ADMIN_PORT@/$admin_port/g" \
        -e "s/@IO_WORKERS@/$io_workers/g" \
        "$input" > "$output"
}

wait_http() {
    local url="$1"
    for _ in $(seq 1 100); do
        if curl --noproxy '*' -fsS --max-time 0.2 "$url" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    echo "service did not become ready: $url" >&2
    return 1
}

mkdir -p "$src_dir"
if [[ "$source_mode" == "head" ]]; then
    log "export git HEAD"
    git -C "$repo_root" archive --format=tar HEAD | tar -xf - -C "$src_dir"
else
    log "export tracked worktree"
    git -C "$repo_root" ls-files -z \
        | tar -C "$repo_root" --null -T - -cf - \
        | tar -xf - -C "$src_dir"
    git -C "$repo_root" diff --binary HEAD > "$out_dir/source.patch"
fi

log "configure RelWithDebInfo build"
cmake -S "$src_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO='-O2 -g -DNDEBUG -fno-omit-frame-pointer' \
    > "$out_dir/cmake-configure.log"
log "build gw"
cmake --build "$build_dir" --target gw --parallel "$(nproc)" > "$out_dir/cmake-build.log"

mkdir -p "$src_dir/api_gw/bin" "$src_dir/logs"
cp "$script_dir/configs/bronx.yml" "$src_dir/api_gw/bin/bronx.yml"
render "$script_dir/configs/gateway.yml.in" "$src_dir/api_gw/bin/gateway.yml"
cp "$src_dir/api_gw/bin/bronx.yml" "$out_dir/configs/bronx.yml"
cp "$src_dir/api_gw/bin/gateway.yml" "$out_dir/configs/gateway.yml"

mkdir -p "$upstream_prefix/logs" "$nginx_prefix/logs"
render "$script_dir/configs/nginx-upstream.conf.in" "$upstream_prefix/nginx.conf"
render "$script_dir/configs/nginx-proxy.conf.in" "$nginx_prefix/nginx.conf"
cp "$upstream_prefix/nginx.conf" "$out_dir/configs/nginx-upstream.conf"
cp "$nginx_prefix/nginx.conf" "$out_dir/configs/nginx-proxy.conf"

{
    printf 'date_utc=%s\n' "$(date -u +%FT%TZ)"
    printf 'commit=%s\n' "$(git -C "$repo_root" rev-parse HEAD)"
    printf 'branch=%s\n' "$(git -C "$repo_root" branch --show-current)"
    printf 'source_mode=%s\n' "$source_mode"
    printf 'build_type=RelWithDebInfo\n'
    printf 'cxx_flags=-O2 -g -DNDEBUG -fno-omit-frame-pointer\n'
    printf 'compiler=%s\n' "$(c++ --version | head -1)"
    printf 'vegeta=%s\n' "$(vegeta -version | head -1)"
    printf 'nginx=%s\n' "$(nginx -v 2>&1)"
    printf 'kernel=%s\n' "$(uname -srmo)"
    printf 'online_cpus=%s\n' "$(nproc)"
    printf 'load_cpus=%s\nproxy_cpu=%s\nupstream_cpu=%s\n' \
        "$load_cpus" "$proxy_cpu" "$upstream_cpu"
    printf 'duration_seconds=%s\nrounds=%s\nrates=%s\n' "$duration" "$rounds" "$rates"
    printf 'bronx_rates=%s\nnginx_rates=%s\nio_workers=%s\n' \
        "$bronx_rates" "$nginx_rates" "$io_workers"
    printf 'vegeta_workers=%s\nvegeta_max_workers=%s\nvegeta_connections=%s\n' \
        "$workers" "$max_workers" "$connections"
    printf 'ulimit_n=%s\n' "$(ulimit -n)"
    printf 'binary_sha256=%s\n' "$(sha256sum "$src_dir/bin/gw" | awk '{print $1}')"
} > "$out_dir/metadata.env"
lscpu > "$out_dir/lscpu.txt"
uname -a > "$out_dir/uname.txt"

log "start static nginx upstream on CPU $upstream_cpu"
taskset -c "$upstream_cpu" nginx -p "$upstream_prefix/" -c nginx.conf
upstream_master_pid="$(<"$upstream_prefix/logs/nginx.pid")"
upstream_worker_pid="$(pgrep -P "$upstream_master_pid" | head -1)"
wait_http "http://127.0.0.1:$upstream_port/bench"

log "start Bronx on CPU $proxy_cpu"
taskset -c "$proxy_cpu" "$src_dir/bin/gw" > "$out_dir/bronx.stdout.log" 2>&1 &
bronx_pid=$!
wait_http "http://127.0.0.1:$admin_port/healthz"
wait_http "http://127.0.0.1:$bronx_port/bench"

log "start nginx proxy on CPU $proxy_cpu"
taskset -c "$proxy_cpu" nginx -p "$nginx_prefix/" -c nginx.conf
nginx_master_pid="$(<"$nginx_prefix/logs/nginx.pid")"
nginx_worker_pid="$(pgrep -P "$nginx_master_pid" | head -1)"
wait_http "http://127.0.0.1:$nginx_port/bench"

printf 'label,proxy,target_qps,round,requests,actual_qps,throughput_qps,success,p50_ms,p95_ms,p99_ms,max_ms,proxy_cpu_pct,upstream_cpu_pct,generator_cpu_pct,error_kinds\n' \
    > "$out_dir/summary.csv"

run_attack() {
    local label="$1"
    local proxy="$2"
    local url="$3"
    local rate="$4"
    local round="$5"
    local observed_pid="$6"
    local bin="$work_dir/$label.bin"
    local json="$out_dir/results/$label.json"
    local cpu="$out_dir/results/$label.cpu.txt"
    local target="$work_dir/$label.targets"

    printf 'GET %s\n' "$url" > "$target"
    log "attack label=$label target_qps=$rate duration=${duration}s"
    taskset -c "$load_cpus" vegeta attack \
        -name "$label" -targets "$target" -rate "$rate/s" -duration "${duration}s" \
        -workers "$workers" -max-workers "$max_workers" \
        -connections "$connections" -max-connections "$connections" \
        -timeout 2s -max-body 0 -output "$bin" &
    local attack_pid=$!
    LC_ALL=C pidstat -u -p "$observed_pid,$upstream_worker_pid,$attack_pid" 1 "$duration" > "$cpu" &
    local pidstat_pid=$!
    wait "$attack_pid"
    wait "$pidstat_pid" || true
    vegeta report -type=json "$bin" > "$json"

    local proxy_cpu_pct upstream_cpu_pct generator_cpu_pct
    proxy_cpu_pct="$(awk -v p="$observed_pid" '$1 == "Average:" && $3 == p {v=$8} END {print v+0}' "$cpu")"
    upstream_cpu_pct="$(awk -v p="$upstream_worker_pid" '$1 == "Average:" && $3 == p {v=$8} END {print v+0}' "$cpu")"
    generator_cpu_pct="$(awk -v p="$attack_pid" '$1 == "Average:" && $3 == p {v=$8} END {print v+0}' "$cpu")"
    jq -r --arg label "$label" --arg proxy "$proxy" --arg target "$rate" \
        --arg round "$round" --arg pcpu "$proxy_cpu_pct" --arg ucpu "$upstream_cpu_pct" \
        --arg gcpu "$generator_cpu_pct" \
        '[ $label, $proxy, $target, $round, .requests, .rate, .throughput,
           .success, (.latencies["50th"] / 1000000), (.latencies["95th"] / 1000000),
           (.latencies["99th"] / 1000000), (.latencies.max / 1000000),
           $pcpu, $ucpu, $gcpu, (.errors | length) ] | @csv' "$json" \
        >> "$out_dir/summary.csv"

    if [[ "$keep_raw" == "1" ]]; then
        gzip -9 "$bin"
        mv "$bin.gz" "$out_dir/results/$label.bin.gz"
    else
        rm -f -- "$bin"
    fi
}

warmup() {
    local label="$1"
    local url="$2"
    local target="$work_dir/warmup-$label.targets"
    printf 'GET %s\n' "$url" > "$target"
    log "warmup $label"
    taskset -c "$load_cpus" vegeta attack -targets "$target" -rate 5000/s \
        -duration "${warmup_duration}s" -workers 64 -max-workers 256 \
        -connections 256 -max-connections 256 -timeout 2s -max-body 0 \
        -output "$work_dir/warmup-$label.bin"
    rm -f -- "$work_dir/warmup-$label.bin"
}

if [[ "$run_direct" == "1" ]]; then
    warmup direct "http://127.0.0.1:$upstream_port/bench"
    run_attack "direct-${direct_rate}-r1" direct \
        "http://127.0.0.1:$upstream_port/bench" "$direct_rate" 1 "$upstream_worker_pid"
fi

for proxy in $proxies; do
    case "$proxy" in
        bronx)
            url="http://127.0.0.1:$bronx_port/bench"
            observed_pid="$bronx_pid"
            proxy_rates="$bronx_rates"
            ;;
        nginx)
            url="http://127.0.0.1:$nginx_port/bench"
            observed_pid="$nginx_worker_pid"
            proxy_rates="$nginx_rates"
            ;;
        *)
            echo "unsupported proxy in PROXIES: $proxy" >&2
            exit 1
            ;;
    esac
    warmup "$proxy" "$url"
    for rate in $proxy_rates; do
        for round in $(seq 1 "$rounds"); do
            run_attack "$proxy-${rate}-r${round}" "$proxy" "$url" "$rate" "$round" "$observed_pid"
        done
    done
done

curl --noproxy '*' -fsS "http://127.0.0.1:$admin_port/stats" \
    > "$out_dir/bronx-final-stats.json"

log "complete out_dir=$out_dir"
(
    cd "$out_dir"
    find . -type f ! -name manifest.sha256 -print0 | sort -z | xargs -0 sha256sum
) > "$out_dir/manifest.sha256"
