#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO_DIR="$ROOT_DIR/demo"
RUN_DIR="$DEMO_DIR/run"
ALL_COMPONENTS=(httpbin1 httpbin2 json-server ws-echo hub gw nginx prometheus loki alloy grafana)
STOP_ORDER=(grafana alloy loki prometheus nginx gw hub ws-echo json-server httpbin2 httpbin1)
REQUESTED=("$@")
SOCKETS=(
    /tmp/bronx_demo_ip_submit.sock
    /tmp/bronx_demo_ip_subscribe.sock
    /tmp/bronx_demo_ip_admin.sock
)

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

process_exists() {
    [[ -n "${1:-}" && -d "/proc/$1" ]]
}

cleanup_hub_sockets() {
    rm -f -- "${SOCKETS[@]}"
}

stop_one() {
    local name="$1" file pid signal deadline
    file="$(pid_file "$name")"
    if ! pid="$(read_pid "$name")"; then
        echo "[$name] already stopped"
        [[ "$name" == "hub" ]] && cleanup_hub_sockets
        [[ -e "$file" ]] && rm -f -- "$file"
        return 0
    fi
    if ! process_exists "$pid"; then
        echo "[$name] removed stale pid=$pid"
        rm -f -- "$file"
        [[ "$name" == "hub" ]] && cleanup_hub_sockets
        return 0
    fi

    signal=TERM
    [[ "$name" == "nginx" ]] && signal=QUIT
    if ! kill -"$signal" "$pid" 2>/dev/null; then
        if [[ "$name" == "nginx" && EUID -ne 0 ]]; then
            echo "[nginx] needs root to stop pid=$pid; run: sudo $DEMO_DIR/down.sh nginx"
            return 0
        fi
        echo "[$name] cannot signal pid=$pid" >&2
        return 1
    fi

    deadline=$((SECONDS + 10))
    while process_exists "$pid" && ((SECONDS < deadline)); do
        sleep 0.1
    done
    if process_exists "$pid"; then
        echo "[$name] did not stop after 10s; sending SIGKILL"
        if ! kill -9 "$pid" 2>/dev/null; then
            echo "[$name] cannot SIGKILL pid=$pid" >&2
            return 1
        fi
        deadline=$((SECONDS + 2))
        while process_exists "$pid" && ((SECONDS < deadline)); do sleep 0.1; done
    fi
    if process_exists "$pid"; then
        echo "[$name] pid=$pid is still present" >&2
        return 1
    fi

    rm -f -- "$file"
    [[ "$name" == "hub" ]] && cleanup_hub_sockets
    echo "[$name] stopped pid=$pid"
}

failed=0
for component in "${STOP_ORDER[@]}"; do
    selected "$component" || continue
    stop_one "$component" || failed=1
done

exit "$failed"
