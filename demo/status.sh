#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="$ROOT_DIR/demo/run"
ALL_COMPONENTS=(httpbin1 httpbin2 json-server ws-echo hub gw nginx prometheus loki alloy grafana)
REQUESTED=("$@")

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

read_pid() {
    local file="$RUN_DIR/$1.pid"
    [[ -r "$file" ]] || return 1
    local pid
    pid="$(<"$file")"
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    printf '%s' "$pid"
}

probe_url() {
    local url="$1" mode="${2:-plain}"
    if [[ "$mode" == "insecure" ]]; then
        curl -kfsS --max-time 1 "$url" >/dev/null 2>&1
    else
        curl -fsS --max-time 1 "$url" >/dev/null 2>&1
    fi
}

healthy() {
    case "$1" in
        httpbin1) probe_url http://127.0.0.1:8081/status/200 ;;
        httpbin2) probe_url http://127.0.0.1:8082/status/200 ;;
        json-server) probe_url http://127.0.0.1:3002/products ;;
        ws-echo) probe_url http://127.0.0.1:3003/healthz ;;
        hub)
            probe_url http://127.0.0.1:9091/healthz \
                && [[ -S /tmp/bronx_demo_ip_submit.sock ]] \
                && [[ -S /tmp/bronx_demo_ip_subscribe.sock ]] \
                && [[ -S /tmp/bronx_demo_ip_admin.sock ]]
            ;;
        gw) probe_url http://127.0.0.1:9090/healthz ;;
        nginx) probe_url https://127.0.0.1/healthz insecure ;;
        prometheus) probe_url http://127.0.0.1:9099/-/ready ;;
        loki) probe_url http://127.0.0.1:3100/ready ;;
        alloy) probe_url http://127.0.0.1:12345/-/ready ;;
        grafana) probe_url http://127.0.0.1:3000/api/health ;;
    esac
}

port_label() {
    case "$1" in
        httpbin1) echo 8081 ;;
        httpbin2) echo 8082 ;;
        json-server) echo 3002 ;;
        ws-echo) echo 3003 ;;
        hub) echo '9091 + UDS' ;;
        gw) echo '8090/9090' ;;
        nginx) echo '80/443' ;;
        prometheus) echo 9099 ;;
        loki) echo 3100 ;;
        alloy) echo 12345 ;;
        grafana) echo 3000 ;;
    esac
}

printf '%-13s %-8s %-12s %-11s %s\n' COMPONENT PID PORT PROCESS HEALTH
printf '%-13s %-8s %-12s %-11s %s\n' ------------- -------- ------------ ----------- ------
for component in "${ALL_COMPONENTS[@]}"; do
    selected "$component" || continue
    pid="-"
    process=stopped
    health=DOWN
    if value="$(read_pid "$component")"; then
        pid="$value"
        if [[ -d "/proc/$pid" ]]; then
            process=running
        else
            process=stale-pid
        fi
    fi
    if healthy "$component"; then
        health=UP
        [[ "$process" == stopped ]] && process=unmanaged
    fi
    printf '%-13s %-8s %-12s %-11s %s\n' \
        "$component" "$pid" "$(port_label "$component")" "$process" "$health"
done
