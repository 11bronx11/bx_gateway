#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO_DIR="$ROOT_DIR/demo"
RUN_DIR="$DEMO_DIR/run"
LOG_DIR="$DEMO_DIR/logs"
ALL_COMPONENTS=(httpbin1 httpbin2 json-server ws-echo hub gw nginx prometheus loki alloy grafana)
REQUESTED=("$@")
SOCKETS=(
    /tmp/bronx_demo_ip_submit.sock
    /tmp/bronx_demo_ip_subscribe.sock
    /tmp/bronx_demo_ip_admin.sock
)

mkdir -p "$RUN_DIR" "$LOG_DIR"

usage() {
    echo "usage: $0 [httpbin1|httpbin2|json-server|ws-echo|hub|gw|nginx|prometheus|loki|alloy|grafana ...]"
}

is_known() {
    local wanted="$1" component
    for component in "${ALL_COMPONENTS[@]}"; do
        [[ "$component" == "$wanted" ]] && return 0
    done
    return 1
}

selected() {
    local wanted="$1" component
    ((${#REQUESTED[@]} == 0)) && return 0
    for component in "${REQUESTED[@]}"; do
        [[ "$component" == "$wanted" ]] && return 0
    done
    return 1
}

for component in "${REQUESTED[@]}"; do
    if ! is_known "$component"; then
        echo "unknown component: $component" >&2
        usage >&2
        exit 2
    fi
done

pid_file() {
    printf '%s/%s.pid' "$RUN_DIR" "$1"
}

read_pid() {
    local file
    file="$(pid_file "$1")"
    [[ -r "$file" ]] || return 1
    local pid
    pid="$(<"$file")"
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    printf '%s' "$pid"
}

is_running() {
    local pid
    pid="$(read_pid "$1")" || return 1
    [[ -d "/proc/$pid" ]]
}

clear_stale_pid() {
    local name="$1" file
    file="$(pid_file "$name")"
    if [[ -e "$file" ]] && ! is_running "$name"; then
        rm -f -- "$file"
    fi
}

url_up() {
    local url="$1" mode="${2:-plain}"
    if [[ "$mode" == "insecure" ]]; then
        curl -kfsS --max-time 1 "$url" >/dev/null 2>&1
    else
        curl -fsS --max-time 1 "$url" >/dev/null 2>&1
    fi
}

wait_up() {
    local name="$1" url="$2" timeout="$3" mode="${4:-plain}"
    local deadline=$((SECONDS + timeout))
    while ((SECONDS < deadline)); do
        if url_up "$url" "$mode"; then
            echo "[$name] ready: $url"
            return 0
        fi
        if ! is_running "$name"; then
            echo "[$name] exited before becoming ready; see $LOG_DIR/$name.log" >&2
            return 1
        fi
        sleep 0.1
    done
    echo "[$name] readiness timed out after ${timeout}s: $url" >&2
    echo "[$name] log: $LOG_DIR/$name.log" >&2
    return 1
}

wait_hub_sockets() {
    local deadline=$((SECONDS + 10)) socket ready
    while ((SECONDS < deadline)); do
        ready=1
        for socket in "${SOCKETS[@]}"; do
            [[ -S "$socket" ]] || ready=0
        done
        if ((ready)); then
            echo "[hub] submit, subscribe, and admin UDS are ready"
            return 0
        fi
        is_running hub || return 1
        sleep 0.1
    done
    echo "[hub] UDS readiness timed out; see $LOG_DIR/hub.log" >&2
    return 1
}

stop_failed_start() {
    local name="$1" pid
    pid="$(read_pid "$name")" || return 0
    kill "$pid" 2>/dev/null || true
    local deadline=$((SECONDS + 5))
    while [[ -d "/proc/$pid" ]] && ((SECONDS < deadline)); do sleep 0.1; done
    [[ -d "/proc/$pid" ]] && kill -9 "$pid" 2>/dev/null || true
    rm -f -- "$(pid_file "$name")"
}

start_background() {
    local name="$1" cwd="$2"
    shift 2
    clear_stale_pid "$name"
    (
        cd -- "$cwd" || exit 1
        nohup "$@" >>"$LOG_DIR/$name.log" 2>&1 </dev/null &
        printf '%s\n' "$!" >"$(pid_file "$name")"
    ) || return 1
    if ! is_running "$name"; then
        echo "[$name] failed to start; see $LOG_DIR/$name.log" >&2
        return 1
    fi
    echo "[$name] started pid=$(read_pid "$name")"
}

prepare_binaries() {
    local binary
    mkdir -p "$DEMO_DIR/bin" "$DEMO_DIR/api_gw/bin/db"
    for binary in gw hub banctl; do
        if [[ ! -x "$ROOT_DIR/bin/$binary" ]]; then
            echo "missing $ROOT_DIR/bin/$binary; build the project first" >&2
            return 1
        fi
        install -m 0755 "$ROOT_DIR/bin/$binary" "$DEMO_DIR/bin/$binary"
    done
}

start_httpbin() {
    local name="$1" port="$2"
    local gunicorn="$DEMO_DIR/upstream/venv/bin/gunicorn"
    if [[ ! -x "$gunicorn" ]]; then
        echo "missing $gunicorn; install the Stage 0 venv dependencies" >&2
        return 1
    fi
    start_background "$name" "$DEMO_DIR/upstream" \
        "$gunicorn" -w 2 -b "127.0.0.1:$port" httpbin:app || return 1
    if ! wait_up "$name" "http://127.0.0.1:$port/status/200" 15; then
        stop_failed_start "$name"
        return 1
    fi
}

start_json_server() {
    local json_server="$DEMO_DIR/upstream/node_modules/.bin/json-server"
    if [[ ! -x "$json_server" ]]; then
        echo "missing $json_server; run npm install in demo/upstream" >&2
        return 1
    fi
    start_background json-server "$DEMO_DIR/upstream" \
        "$json_server" db.json --port 3002 --host 127.0.0.1 || return 1
    if ! wait_up json-server http://127.0.0.1:3002/products 15; then
        stop_failed_start json-server
        return 1
    fi
}

start_ws_echo() {
    local node=/usr/bin/node server="$DEMO_DIR/upstream/ws_echo.js"
    [[ -x "$node" ]] || { echo "missing $node" >&2; return 1; }
    [[ -r "$server" ]] || { echo "missing $server" >&2; return 1; }
    [[ -r "$DEMO_DIR/upstream/node_modules/ws/package.json" ]] || {
        echo "missing ws dependency; run npm install in demo/upstream" >&2
        return 1
    }
    start_background ws-echo "$DEMO_DIR/upstream" "$node" "$server" || return 1
    if ! wait_up ws-echo http://127.0.0.1:3003/healthz 15; then
        stop_failed_start ws-echo
        return 1
    fi
}

start_hub() {
    if url_up http://127.0.0.1:9091/healthz; then
        echo "[hub] port 9091 is served by a process not tracked in demo/run" >&2
        return 1
    fi
    rm -f -- "${SOCKETS[@]}"
    start_background hub "$DEMO_DIR" ./bin/hub || return 1
    if ! wait_up hub http://127.0.0.1:9091/healthz 15 || ! wait_hub_sockets; then
        stop_failed_start hub
        rm -f -- "${SOCKETS[@]}"
        return 1
    fi
}

start_gw() {
    if ! is_running hub; then
        echo "[gw] hub is not managed and running; start hub first" >&2
        return 1
    fi
    if url_up http://127.0.0.1:9090/healthz; then
        echo "[gw] port 9090 is served by a process not tracked in demo/run" >&2
        return 1
    fi
    start_background gw "$DEMO_DIR" ./bin/gw || return 1
    if ! wait_up gw http://127.0.0.1:9090/healthz 20; then
        stop_failed_start gw
        return 1
    fi
}

start_nginx() {
    if ((EUID != 0)); then
        echo "[nginx] needs root for ports 80/443; run: sudo $DEMO_DIR/up.sh nginx"
        return 0
    fi
    local nginx=/usr/sbin/nginx config="$DEMO_DIR/nginx/gw.conf"
    [[ -x "$nginx" ]] || { echo "missing $nginx" >&2; return 1; }
    "$nginx" -t -p "$DEMO_DIR/nginx/" -c "$config" || return 1
    "$nginx" -p "$DEMO_DIR/nginx/" -c "$config" || return 1
    local deadline=$((SECONDS + 5))
    while ((SECONDS < deadline)); do
        is_running nginx && break
        sleep 0.1
    done
    if ! is_running nginx || ! wait_up nginx https://127.0.0.1/healthz 10 insecure; then
        echo "[nginx] failed to start; see $RUN_DIR/nginx_error.log" >&2
        stop_failed_start nginx
        return 1
    fi
}

start_prometheus() {
    local prometheus=/usr/bin/prometheus
    [[ -x "$prometheus" ]] || { echo "missing $prometheus" >&2; return 1; }
    mkdir -p "$RUN_DIR/prom-data"
    start_background prometheus "$ROOT_DIR" \
        "$prometheus" \
        --config.file="$DEMO_DIR/prometheus/prometheus.yml" \
        --storage.tsdb.path="$RUN_DIR/prom-data" \
        --web.listen-address=127.0.0.1:9099 || return 1
    if ! wait_up prometheus http://127.0.0.1:9099/-/ready 20; then
        stop_failed_start prometheus
        return 1
    fi
}

start_loki() {
    local loki=/usr/bin/loki
    [[ -x "$loki" ]] || { echo "missing $loki; install the loki package" >&2; return 1; }
    mkdir -p "$RUN_DIR/loki-data"
    start_background loki "$ROOT_DIR" \
        "$loki" -config.file="$DEMO_DIR/loki/loki.yml" || return 1
    if ! wait_up loki http://127.0.0.1:3100/ready 30; then
        stop_failed_start loki
        return 1
    fi
}

start_alloy() {
    local alloy=/usr/bin/alloy
    [[ -x "$alloy" ]] || { echo "missing $alloy; install the alloy package" >&2; return 1; }
    if ! is_running loki; then
        echo "[alloy] Loki is not managed and running; start Loki first" >&2
        return 1
    fi
    mkdir -p "$RUN_DIR/alloy-data"
    start_background alloy "$ROOT_DIR" \
        "$alloy" run \
        --server.http.listen-addr=127.0.0.1:12345 \
        --storage.path="$RUN_DIR/alloy-data" \
        "$DEMO_DIR/alloy/config.alloy" || return 1
    if ! wait_up alloy http://127.0.0.1:12345/-/ready 30; then
        stop_failed_start alloy
        return 1
    fi
}

wait_grafana_assets() {
    local deadline=$((SECONDS + 20))
    while ((SECONDS < deadline)); do
        if curl -fsS -u admin:admin --max-time 2 \
            http://127.0.0.1:3000/api/datasources/uid/bronx-prometheus/health >/dev/null 2>&1 \
            && curl -fsS -u admin:admin --max-time 2 \
            http://127.0.0.1:3000/api/datasources/uid/bronx-loki/health >/dev/null 2>&1 \
            && curl -fsS -u admin:admin --max-time 2 \
            http://127.0.0.1:3000/api/dashboards/uid/bronx-gateway-demo >/dev/null 2>&1; then
            echo "[grafana] Prometheus, Loki, and Bronx Gateway Demo dashboard are provisioned"
            return 0
        fi
        is_running grafana || return 1
        sleep 0.2
    done
    echo "[grafana] provisioning timed out; see $LOG_DIR/grafana.log" >&2
    return 1
}

start_grafana() {
    local grafana=/usr/share/grafana/bin/grafana
    [[ -x "$grafana" ]] || { echo "missing $grafana" >&2; return 1; }
    mkdir -p "$RUN_DIR/grafana-data" "$RUN_DIR/grafana-logs" "$RUN_DIR/grafana-plugins"
    start_background grafana "$ROOT_DIR" env \
        GF_SERVER_HTTP_ADDR=127.0.0.1 \
        GF_SERVER_HTTP_PORT=3000 \
        GF_PATHS_DATA="$RUN_DIR/grafana-data" \
        GF_PATHS_LOGS="$RUN_DIR/grafana-logs" \
        GF_PATHS_PLUGINS="$RUN_DIR/grafana-plugins" \
        GF_PATHS_PROVISIONING="$DEMO_DIR/grafana/provisioning" \
        GF_SECURITY_ADMIN_USER=admin \
        GF_SECURITY_ADMIN_PASSWORD=admin \
        GF_AUTH_ANONYMOUS_ENABLED=true \
        GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer \
        BRONX_DEMO_GRAFANA_DIR="$DEMO_DIR/grafana" \
        "$grafana" server --homepath /usr/share/grafana --config /dev/null || return 1
    if ! wait_up grafana http://127.0.0.1:3000/api/health 30 || ! wait_grafana_assets; then
        stop_failed_start grafana
        return 1
    fi
}

start_one() {
    local name="$1"
    clear_stale_pid "$name"
    if is_running "$name"; then
        echo "[$name] already running pid=$(read_pid "$name")"
        return 0
    fi
    case "$name" in
        httpbin1)  start_httpbin httpbin1 8081 ;;
        httpbin2)  start_httpbin httpbin2 8082 ;;
        json-server) start_json_server ;;
        ws-echo)   start_ws_echo ;;
        hub)       start_hub ;;
        gw)        start_gw ;;
        nginx)     start_nginx ;;
        prometheus) start_prometheus ;;
        loki)      start_loki ;;
        alloy)     start_alloy ;;
        grafana)   start_grafana ;;
    esac
}

if selected hub || selected gw; then
    prepare_binaries || exit 1
fi

for component in "${ALL_COMPONENTS[@]}"; do
    selected "$component" || continue
    start_one "$component" || exit 1
done

echo "demo startup pass complete; run: $DEMO_DIR/status.sh"
