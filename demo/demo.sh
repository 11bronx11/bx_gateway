#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO_DIR="$ROOT_DIR/demo"
BASE_URL=https://gw.local
ADMIN_URL=http://127.0.0.1:9090
HUB_URL=http://127.0.0.1:9091
LOKI_URL=http://127.0.0.1:3100
CA_FILE="$DEMO_DIR/nginx/ca/ca.crt"
ACCESS_LOG="$DEMO_DIR/logs/gw_system.log"
TRACE_LOG="$DEMO_DIR/logs/gw_trace.log"
BANCTL="$DEMO_DIR/bin/banctl"
TLS_ARGS=(--silent --show-error --cacert "$CA_FILE" --resolve gw.local:443:127.0.0.1)

declare -A TITLES=(
    [1]='TLS and HTTP/2'
    [2]='request ID propagation'
    [3]='trusted X-Forwarded-For'
    [4]='routing and prefix rewrite'
    [5]='JWT authorization states'
    [6]='per-route rate limiting'
    [7]='WAF signatures'
    [8]='IP-ban control plane and persistence'
    [9]='weighted load balancing'
    [10]='circuit breaker recovery'
    [11]='SSE streaming'
    [12]='open-loop load and Grafana'
    [13]='Perfetto trace conversion'
    [14]='hot reload with an in-flight stream'
    [15]='WSS tunnel and frame roundtrip'
    [16]='Loki log ingestion and request correlation'
)

usage() {
    echo "usage: $0 [1-16]"
    local n
    for n in $(seq 1 16); do printf '  %2d  %s\n' "$n" "${TITLES[$n]}"; done
}

if (($# > 1)); then usage >&2; exit 2; fi
if (($# == 1)) && { [[ ! "$1" =~ ^[0-9]+$ ]] || ((10#$1 < 1 || 10#$1 > 16)); }; then
    usage >&2
    exit 2
fi

https_code() {
    local path="$1"
    shift
    curl "${TLS_ARGS[@]}" --output /dev/null --write-out '%{http_code}' \
        "$@" "$BASE_URL$path" 2>/dev/null || true
}

expect_code() {
    local expected="$1" path="$2"
    shift 2
    local actual
    actual="$(https_code "$path" "$@")"
    printf '  %-38s expected=%s actual=%s\n' "$path" "$expected" "$actual"
    [[ "$actual" == "$expected" ]]
}

wait_log() {
    local file="$1" needle="$2" timeout="${3:-8}"
    local deadline=$((SECONDS + timeout))
    while ((SECONDS < deadline)); do
        [[ -f "$file" ]] && /usr/bin/grep -Fq -- "$needle" "$file" && return 0
        sleep 0.2
    done
    return 1
}

banctl() {
    BRONX_IP_ADMIN_SOCK=/tmp/bronx_demo_ip_admin.sock "$BANCTL" "$@"
}

ban_has_ip() {
    local ip="$1"
    banctl --json list 2>/dev/null |
        jq -e --arg ip "$ip" '.ok and any(.rules[]?; .ip == $ip)' >/dev/null
}

wait_ban_ip() {
    local ip="$1" timeout="${2:-8}"
    local deadline=$((SECONDS + timeout))
    while ((SECONDS < deadline)); do
        ban_has_ip "$ip" && return 0
        sleep 0.2
    done
    return 1
}

clear_ip_rules() {
    local ip="$1" ids id
    ids="$(banctl --json list 2>/dev/null |
        jq -r --arg ip "$ip" '.rules[]? | select(.ip == $ip) | .id')"
    while IFS= read -r id; do
        [[ -n "$id" ]] && banctl del "$id" >/dev/null 2>&1 || true
    done <<<"$ids"
    banctl unban "$ip/32" >/dev/null 2>&1 || true
}

trace_on() {
    local path="${1:-}"
    local url="$ADMIN_URL/trace/on?level=2&ttl=120"
    [[ -n "$path" ]] && url+="&path=$path"
    curl -fsS -X POST "$url" >/dev/null
}

trace_off() {
    curl -fsS -X POST "$ADMIN_URL/trace/off" >/dev/null 2>&1 || true
}

point_1() {
    local redirect secure
    redirect="$(curl -sS --resolve gw.local:80:127.0.0.1 -o /dev/null -w '%{http_code}' \
        http://gw.local/healthz)"
    secure="$(curl "${TLS_ARGS[@]}" --http2 -o /dev/null -w '%{http_code} %{http_version}' \
        "$BASE_URL/healthz")"
    echo "  HTTP redirect: $redirect"
    echo "  verified TLS response: $secure"
    [[ "$redirect" == 301 && "$secure" == '200 2' ]]
}

point_2() {
    local headers request_id
    headers="$(curl "${TLS_ARGS[@]}" -D - -o /dev/null "$BASE_URL/api/get")" || return 1
    request_id="$(awk 'tolower($1)=="x-request-id:" {gsub("\\r", "", $2); print $2}' \
        <<<"$headers" | tail -1)"
    [[ -n "$request_id" ]] || { echo '  response has no X-Request-Id' >&2; return 1; }
    if ! wait_log "$ACCESS_LOG" "\"request_id\":\"$request_id\""; then
        echo "  request ID not found in $ACCESS_LOG" >&2
        return 1
    fi
    echo "  request ID reached response and access log: $request_id"
}

point_3() {
    local observed_ip=203.0.113.9 headers request_id line direct_id direct_line
    headers="$(curl "${TLS_ARGS[@]}" -H "X-Forwarded-For: $observed_ip" \
        -D - -o /dev/null "$BASE_URL/api/get")" || return 1
    request_id="$(awk 'tolower($1)=="x-request-id:" {gsub("\\r", "", $2); print $2}' \
        <<<"$headers" | tail -1)"
    wait_log "$ACCESS_LOG" "\"request_id\":\"$request_id\"" || return 1
    line="$(/usr/bin/grep -F "\"request_id\":\"$request_id\"" "$ACCESS_LOG" | tail -1)"

    direct_id="$(printf '%032x' "$$")"
    curl -fsS --interface 127.0.0.2 -H 'X-Forwarded-For: 1.2.3.4' \
        -H "X-Request-Id: $direct_id" \
        -o /dev/null http://127.0.0.1:8090/api/get || return 1
    wait_log "$ACCESS_LOG" "\"request_id\":\"$direct_id\"" || return 1
    direct_line="$(/usr/bin/grep -F "\"request_id\":\"$direct_id\"" "$ACCESS_LOG" | tail -1)"
    echo "  via nginx observed $observed_ip; untrusted direct peer remained 127.0.0.2"
    [[ "$line" == *"\"ip\":\"$observed_ip\""* \
        && "$direct_line" == *'"ip":"127.0.0.2"'* ]]
}

point_4() {
    local api shop missing cache
    api="$(curl "${TLS_ARGS[@]}" "$BASE_URL/api/get")" || return 1
    shop="$(curl "${TLS_ARGS[@]}" "$BASE_URL/shop/products")" || return 1
    missing="$(https_code /nope)"
    cache="$(curl -fsS "$ADMIN_URL/stats")" || return 1
    echo "  /api/get rewrote to $(jq -r '.url' <<<"$api")"
    echo "  /shop/products returned $(jq 'length' <<<"$shop") records; /nope=$missing"
    jq -e '.url | endswith("/get")' <<<"$api" >/dev/null \
        && jq -e 'type == "array" and length > 0' <<<"$shop" >/dev/null \
        && [[ "$missing" == 404 ]] \
        && jq -e '.route_cache.hits > 0' <<<"$cache" >/dev/null
}

point_5() {
    local good expired alg_none no_scope ok=0
    good="$(python3 "$DEMO_DIR/tools/mkjwt.py" good)"
    expired="$(python3 "$DEMO_DIR/tools/mkjwt.py" expired)"
    alg_none="$(python3 "$DEMO_DIR/tools/mkjwt.py" alg-none)"
    no_scope="$(python3 "$DEMO_DIR/tools/mkjwt.py" no-scope)"
    expect_code 401 /secure/get || ok=1
    expect_code 200 /secure/get -H "Authorization: Bearer $good" || ok=1
    expect_code 401 /secure/get -H "Authorization: Bearer $expired" || ok=1
    expect_code 401 /secure/get -H "Authorization: Bearer $alg_none" || ok=1
    expect_code 403 /secure/get -H "Authorization: Bearer $no_scope" || ok=1
    return "$ok"
}

point_6() {
    local ip="203.0.113.$((40 + $$ % 80))" i code headers ok=0
    local codes=()
    for i in $(seq 1 7); do
        code="$(https_code /limited/get -H "X-Forwarded-For: $ip")"
        codes+=("$code")
    done
    echo "  statuses: ${codes[*]}"
    for i in $(seq 0 4); do [[ "${codes[$i]}" == 200 ]] || ok=1; done
    [[ "${codes[5]}" == 429 && "${codes[6]}" == 429 ]] || ok=1
    headers="$(curl "${TLS_ARGS[@]}" -H "X-Forwarded-For: $ip" -D - -o /dev/null \
        "$BASE_URL/limited/get")" || true
    awk 'tolower($1)=="retry-after:" {found=1} END {exit !found}' <<<"$headers" || ok=1
    return "$ok"
}

point_7() {
    local base=$((130 + $$ % 50)) ok=0 ip code
    local paths=(
        '/api/get?q=%3Cscript%3Ealert(1)%3C%2Fscript%3E'
        '/api/get?q=..%2F..%2Fetc%2Fpasswd'
        '/api/get'
    )
    local agents=('curl/demo' 'curl/demo' 'sqlmap/1.7')
    local labels=(xss traversal scanner)
    local i
    for i in 0 1 2; do
        ip="203.0.113.$((base + i))"
        clear_ip_rules "$ip"
        code="$(https_code "${paths[$i]}" -H "X-Forwarded-For: $ip" -A "${agents[$i]}")"
        echo "  ${labels[$i]} => $code"
        [[ "$code" == 403 ]] || ok=1
        wait_ban_ip "$ip" 8 || ok=1
        clear_ip_rules "$ip"
    done
    return "$ok"
}

wait_gateway_sync() {
    local deadline=$((SECONDS + 15))
    while ((SECONDS < deadline)); do
        curl -fsS "$ADMIN_URL/stats" 2>/dev/null |
            jq -e '.ipban_synced == 1 and .ipban_link_state == 4' >/dev/null && return 0
        sleep 0.2
    done
    return 1
}

point_8() {
    local base=$((20 + $$ % 30))
    local ip="198.51.100.$base"
    local permanent="198.51.100.$((base + 1))" temporary="198.51.100.$((base + 2))"
    local before start_code waf_code denied_code ok=0 trace_slice
    clear_ip_rules "$ip"
    wait_gateway_sync || return 1
    before=$(wc -l <"$TRACE_LOG" 2>/dev/null || echo 0)
    trace_on /api || return 1
    start_code="$(https_code /api/get -H "X-Forwarded-For: $ip")"
    waf_code="$(https_code /api/get -H "X-Forwarded-For: $ip" -A sqlmap/1.7)"
    wait_ban_ip "$ip" 8 || ok=1
    local deadline=$((SECONDS + 8))
    denied_code=000
    while ((SECONDS < deadline)); do
        denied_code="$(https_code /api/get -H "X-Forwarded-For: $ip")"
        [[ "$denied_code" == 403 ]] && break
        sleep 0.2
    done
    sleep 5
    trace_off
    trace_slice="$(tail -n "+$((before + 1))" "$TRACE_LOG")"
    echo "  WAF lifecycle: clean=$start_code waf=$waf_code guard=$denied_code"
    [[ "$start_code" == 200 && "$waf_code" == 403 && "$denied_code" == 403 ]] || ok=1
    [[ "$trace_slice" == *'mw 10 waf  SHORT'* ]] || ok=1
    [[ "$trace_slice" == *'mw 6 ipban  SHORT'* ]] || ok=1
    curl -fsS "$HUB_URL/metrics" | /usr/bin/grep -E \
        'ipban_hub_(rules|version)|ipban_hub_risks_total\{result="ok",source="waf"\}' \
        || ok=1
    clear_ip_rules "$ip"

    clear_ip_rules "$permanent"
    clear_ip_rules "$temporary"
    banctl deny "$permanent/32" perm 'demo persistent rule' >/dev/null || ok=1
    banctl deny "$temporary/32" 30s 'demo volatile rule' >/dev/null || ok=1
    "$DEMO_DIR/down.sh" hub >/dev/null || ok=1
    "$DEMO_DIR/up.sh" hub >/dev/null || ok=1
    wait_gateway_sync || ok=1
    ban_has_ip "$permanent" || ok=1
    ban_has_ip "$temporary" || ok=1
    echo "  hub restart kept permanent and 30s admin rules (admin overrides are always durable)"
    clear_ip_rules "$permanent"
    clear_ip_rules "$temporary"
    return "$ok"
}

point_9() {
    local before primary secondary i ok=0
    before=$(wc -l <"$TRACE_LOG" 2>/dev/null || echo 0)
    trace_on /api/get || return 1
    for i in $(seq 1 40); do
        expect_code 200 /api/get >/dev/null || ok=1
    done
    sleep 5
    trace_off
    read -r primary secondary < <(
        tail -n "+$((before + 1))" "$TRACE_LOG" |
            awk '/up[[:space:]]+acquire/ {
                if ($0 ~ /ep=127.0.0.1:8081/) a++;
                if ($0 ~ /ep=127.0.0.1:8082/) b++
            } END {print a+0, b+0}'
    )
    echo "  endpoint selections: 8081=$primary 8082=$secondary"
    [[ $((primary + secondary)) -eq 40 && "$primary" -gt "$secondary" && "$secondary" -gt 0 ]] || ok=1
    return "$ok"
}

circuit_state() {
    local port="$1"
    curl -fsS "$ADMIN_URL/stats" |
        jq -r --arg endpoint "127.0.0.1:$port" \
            '.upstreams[] | select(.endpoint == $endpoint) | .circuit.state'
}

point_10() {
    local i state opened=0 saw_half=0 closed=0
    "$DEMO_DIR/down.sh" httpbin2 >/dev/null || return 1
    for i in $(seq 1 40); do https_code /api/get >/dev/null; done
    state="$(circuit_state 8082)"
    echo "  after failures: $state"
    [[ "$state" == open ]] && opened=1

    if ! "$DEMO_DIR/up.sh" httpbin2 >/dev/null; then
        echo '  failed to restart httpbin2' >&2
        return 1
    fi
    sleep 5.2
    for i in $(seq 1 30); do
        https_code /api/get >/dev/null
        state="$(circuit_state 8082)"
        [[ "$state" == half_open ]] && saw_half=1
        if [[ "$state" == closed ]]; then closed=1; break; fi
        sleep 0.1
    done
    echo "  recovery observed: half_open=$saw_half final=$state"
    ((opened && saw_half && closed))
}

point_11() {
    local body lines
    body="$(curl "${TLS_ARGS[@]}" -N "$BASE_URL/api/stream/10")" || return 1
    lines="$(awk 'NF {n++} END {print n+0}' <<<"$body")"
    echo "  streamed records: $lines; nginx proxy_buffering is off"
    [[ "$lines" -eq 10 ]] \
        && /usr/bin/grep -Fq 'proxy_buffering off;' "$DEMO_DIR/nginx/gw.conf"
}

point_12() {
    "$DEMO_DIR/tools/load.sh" 30s || return 1
    curl -fsS http://127.0.0.1:9099/api/v1/targets |
        jq -e '[.data.activeTargets[] | select(.health == "up")] | length >= 2' >/dev/null \
        || return 1
    curl -fsS -u admin:admin \
        http://127.0.0.1:3000/api/dashboards/uid/bronx-gateway-demo |
        jq -e '.dashboard.uid == "bronx-gateway-demo" and (.dashboard.panels | length) == 10' >/dev/null
}

point_13() {
    mkdir -p "$DEMO_DIR/out"
    trace_on /api/delay/1 || return 1
    expect_code 200 /api/delay/1 || { trace_off; return 1; }
    sleep 5
    trace_off
    python3 "$DEMO_DIR/tools/trace2perfetto.py" "$TRACE_LOG" \
        -o "$DEMO_DIR/out/trace.json" --last 1 --check || return 1
    local spans
    spans="$(jq '[.traceEvents[] | select(.cat == "mw")] | length' \
        "$DEMO_DIR/out/trace.json")"
    echo "  wrote $spans middleware spans to demo/out/trace.json"
    [[ "$spans" -eq 16 ]]
}

point_14() {
    curl "${TLS_ARGS[@]}" -N -o /dev/null \
        "$BASE_URL/api/drip?duration=2&numbytes=5&delay=0" &
    local stream_pid=$!
    sleep 0.2
    local reload_code
    reload_code="$(curl -sS -X POST -o /dev/null -w '%{http_code}' "$ADMIN_URL/reload")"
    wait "$stream_pid"
    local stream_rc=$?
    local new_code
    new_code="$(https_code /api/get)"
    echo "  reload=$reload_code in-flight=$stream_rc new-request=$new_code"
    [[ "$reload_code" == 200 && "$stream_rc" -eq 0 && "$new_code" == 200 ]]
}

point_15() {
    local before after probe deadline
    before="$(curl -fsS "$ADMIN_URL/stats")" || return 1
    probe="$(node "$DEMO_DIR/tools/ws_probe.js")" || return 1
    deadline=$((SECONDS + 5))
    while ((SECONDS < deadline)); do
        after="$(curl -fsS "$ADMIN_URL/stats")" || return 1
        if jq -e --argjson opened "$(jq '.ws_open' <<<"$before")" \
            --argjson closed "$(jq '.ws_close' <<<"$before")" \
            '.ws_open >= ($opened + 1) and .ws_close >= ($closed + 1) and .ws_active == 0' \
            <<<"$after" >/dev/null; then
            echo "  $probe"
            echo "  metrics: ws_open=$(jq '.ws_open' <<<"$after") ws_close=$(jq '.ws_close' <<<"$after") ws_active=0"
            jq -e '.upgrade == 101 and .text_echo and .binary_echo and .pong and .close_code == 1000' \
                <<<"$probe" >/dev/null
            return
        fi
        sleep 0.1
    done
    echo "  WebSocket metrics did not settle after probe" >&2
    return 1
}

point_16() {
    local headers request_id query result services deadline
    curl -fsS --max-time 2 "$LOKI_URL/ready" >/dev/null || {
        echo "  Loki is not ready; start Loki and Alloy with demo/up.sh" >&2
        return 1
    }

    headers="$(curl "${TLS_ARGS[@]}" -D - -o /dev/null "$BASE_URL/api/get")" || return 1
    request_id="$(awk 'tolower($1)=="x-request-id:" {gsub("\\r", "", $2); print $2}' \
        <<<"$headers" | tail -1)"
    [[ "$request_id" =~ ^[[:xdigit:]]{32}$ ]] || {
        echo "  response has no valid X-Request-Id" >&2
        return 1
    }

    query="{service=~\"nginx|gateway\",log_type=\"access\"} |= \"$request_id\""
    deadline=$((SECONDS + 15))
    result=
    while ((SECONDS < deadline)); do
        result="$(curl -fsS -G "$LOKI_URL/loki/api/v1/query_range" \
            --data-urlencode "query=$query" \
            --data-urlencode 'since=2m' \
            --data-urlencode 'limit=100' 2>/dev/null)" || true
        if jq -e \
            '([.data.result[].stream.service] | unique | sort) == ["gateway", "nginx"]' \
            <<<"$result" >/dev/null 2>&1; then
            break
        fi
        sleep 0.2
    done

    jq -e '([.data.result[].stream.service] | unique | sort) == ["gateway", "nginx"]' \
        <<<"$result" >/dev/null 2>&1 || {
        echo "  request_id did not reach both Nginx and gateway Loki streams" >&2
        return 1
    }
    jq -e '[.data.result[].values[][1] | select(contains("\"status\":200"))] | length >= 2' \
        <<<"$result" >/dev/null || return 1

    services="$(jq -r '[.data.result[].stream.service] | unique | sort | join(",")' \
        <<<"$result")"
    curl -fsS -u admin:admin \
        http://127.0.0.1:3000/api/datasources/uid/bronx-loki/health |
        jq -e '.status == "OK"' >/dev/null || return 1
    curl -fsS -u admin:admin \
        http://127.0.0.1:3000/api/dashboards/uid/bronx-gateway-demo |
        jq -e '[.dashboard.panels[].title] |
            index("Recent Gateway Logs") != null and index("Log Volume") != null' >/dev/null \
        || return 1
    echo "  request_id=$request_id correlated_services=$services status=200"
    echo "  Grafana panels: Recent Gateway Logs, Log Volume"
}

if ! curl -fsS --max-time 2 "$ADMIN_URL/healthz" >/dev/null \
    || ! curl -kfsS --max-time 2 https://127.0.0.1/healthz >/dev/null; then
    echo "demo is not up; run demo/up.sh and start nginx with the printed sudo command" >&2
    exit 1
fi

failures=0
for point in $(seq 1 16); do
    (($# == 1)) && [[ "$point" != "$1" ]] && continue
    printf '\n[%02d] %s\n' "$point" "${TITLES[$point]}"
    if "point_$point"; then
        echo "  PASS"
    else
        echo "  FAIL" >&2
        failures=$((failures + 1))
    fi
done

printf '\ncompleted with %d failure(s)\n' "$failures"
exit "$failures"
