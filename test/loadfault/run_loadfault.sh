#!/usr/bin/env bash
# 阶段二:开环压测 + toxiproxy 故障注入。
# 拓扑: vegeta → 网关:GW_PORT → toxiproxy:TOXI_PROXY_PORT → mock:MOCK_PORT
# 目标:
#   1. baseline — vegeta 恒速率 拿诚实 p50/p99/p999/吞吐 替掉 Python 闭环数
#   2. latency_inject — 上游 +300ms 延迟 看慢熔断触发与恢复
#   3. upstream_down   — 直接切断上游连接 看熔断开→半开→闭全周期
#   4. bandwidth_limit — 限带宽 1KB/s 看连接池驱逐和超时路径
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$script_dir/../.." && pwd -P)"

# ── 二进制路径(可用环境变量覆盖) ───────────────────────────────────────────
GW_BIN="${LF_GW_BIN:-$repo/bin/gw}"
VEGETA_BIN="${LF_VEGETA_BIN:-vegeta}"
TOXIPROXY_SERVER="${LF_TOXI_SERVER:-toxiproxy-server}"
TOXIPROXY_CLI="${LF_TOXI_CLI:-toxiproxy-cli}"
MOCK_PY="$script_dir/mock_upstream.py"
GEN_JWT="$repo/test/gateway/validation/generate_jwt.py"

# ── 可调参数 ────────────────────────────────────────────────────────────────
BASELINE_RPS="${LF_BASELINE_RPS:-200}"      # 开环恒速(req/s)
BASELINE_SECS="${LF_BASELINE_SECS:-60}"
FAULT_RPS="${LF_FAULT_RPS:-100}"
FAULT_SECS="${LF_FAULT_SECS:-30}"
RECOVERY_SECS="${LF_RECOVERY_SECS:-20}"     # 故障解除后观察恢复
JWT_SECRET="${LF_JWT_SECRET:-loadfault-secret}"
JWT_ISSUER="${LF_JWT_ISSUER:-bronx}"
TOXI_API="${LF_TOXI_API:-127.0.0.1:18474}"  # toxiproxy HTTP 管理口(默认)

# ── 运行目录 ────────────────────────────────────────────────────────────────
run_dir="$(mktemp -d /tmp/loadfault.XXXXXX)"
trap 'cleanup' EXIT INT TERM

# ── PID 跟踪 ────────────────────────────────────────────────────────────────
mock_pid=""; toxi_pid=""; gw_pid=""
failures=0

log() { printf '[loadfault] %s\n' "$*" >&2; }
fail() { log "FAIL: $*"; failures=$((failures + 1)); }

stop_pid() {
    local label="$1" pid="${2:-}" timeout="${3:-5}"
    [[ -z "$pid" ]] && return 0
    kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null||true; return 0; }
    kill -TERM "$pid" 2>/dev/null || true
    local end=$((SECONDS + timeout))
    while kill -0 "$pid" 2>/dev/null && (( SECONDS < end )); do sleep 0.1; done
    kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    log "stopped $label"
}

cleanup() {
    local s=$?; trap - EXIT INT TERM
    stop_pid "gateway"      "$gw_pid"   8
    stop_pid "toxiproxy"    "$toxi_pid" 3
    stop_pid "mock"         "$mock_pid" 3
    rm -rf "$run_dir"
    if (( failures > 0 )); then
        log "完成 — $failures 项失败"; exit 1
    fi
    log "完成 — 全部通过"; exit "$s"
}

# ── 端口分配(找空闲 TCP 端口) ───────────────────────────────────────────────
alloc_port() {
    python3 - <<'PY'
import socket
s = socket.socket(); s.bind(("127.0.0.1", 0))
print(s.getsockname()[1]); s.close()
PY
}
MOCK_PORT=$(alloc_port)
TOXI_PROXY_PORT=$(alloc_port)
TOXI_API_PORT=$(alloc_port)
GW_PORT=$(alloc_port)
GW_ADMIN=$(alloc_port)
TOXI_API="127.0.0.1:${TOXI_API_PORT}"
log "端口: mock=$MOCK_PORT toxi_proxy=$TOXI_PROXY_PORT gw=$GW_PORT admin=$GW_ADMIN toxi_api=$TOXI_API_PORT"

# ── 准备 runtime 目录结构(anchorRoot 读 /proc/self/exe 上溯两级当 root) ────────
# exe 在 run_dir/bin/gw → root = run_dir → 读 run_dir/api_gw/bin/gateway.yml
prepare_runtime() {
    mkdir -p "$run_dir/bin" "$run_dir/api_gw/bin" "$run_dir/logs"
    cp "$GW_BIN" "$run_dir/bin/gw"
    # 最小 bronx.yml:只把日志落到 run_dir/logs,不打 stdout 噪音
    cat > "$run_dir/api_gw/bin/bronx.yml" <<'BYML'
cpu_pool:
  threads: 2
  max_queue: 0
  name: cpu
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
    level: warn
    appenders:
      - type: BxFileLogAppender
        file: logs/gw_root.log
        async: true
  - name: gw
    level: warn
    appenders:
      - type: BxFileLogAppender
        file: logs/gw.log
        async: true
BYML
    log "runtime dir ready: $run_dir"
}

# ── 生成最小 gateway.yml ────────────────────────────────────────────────────
write_gw_config() {
python3 - "$run_dir/api_gw/bin/gateway.yml" "$GW_PORT" "$GW_ADMIN" "$TOXI_PROXY_PORT" \
         "$JWT_SECRET" "$JWT_ISSUER" <<'PY'
import sys, pathlib, hashlib, json
cfg, gw_port, admin_port, upstream_port, secret, issuer = sys.argv[1:]
text = f"""server:
  address: "127.0.0.1:{gw_port}"
  admin_address: "127.0.0.1:{admin_port}"
  io_workers: 4
  maintenance: false
trusted_proxies:
  - 127.0.0.1/32
upstreams:
  - name: api
    lb: round_robin
    timeout:
      total_ms: 5000
      connect_ms: 500
      read_ms: 4000
    circuit_breaker:
      failure_threshold: 5
      window_ms: 10000
      min_requests: 10
      failure_rate: 50
      slow_ms: 1000
      slow_rate: 50
      open_timeout_ms: 3000
      max_open_timeout_ms: 8000
      half_open_max_requests: 1
      half_open_successes: 1
      failure_statuses: [500, 502, 503, 504]
    connection_pool:
      max_idle: 32
      idle_timeout_ms: 5000
    endpoints:
      - {{ host: "127.0.0.1", port: {upstream_port} }}
routes:
  - name: public
    path: /public
    match_type: prefix
    upstream: api
    strip_prefix: false
    auth: none
  - name: api
    path: /api
    match_type: prefix
    upstream: api
    strip_prefix: true
    auth: jwt
    require_scopes: [read]
auth:
  jwt:
    enabled: true
    algo: HS256
    secret: {json.dumps(secret)}
    issuer: {json.dumps(issuer)}
  api_key:
    header: "X-API-Key"
    keys: []
  forward:
    enabled: true
    strip_bearer: true
    strip_api_key: true
ip_filter:
  enabled: false
  mode: denylist
  cidrs: []
"""
p = pathlib.Path(cfg)
p.parent.mkdir(parents=True, exist_ok=True)
p.write_text(text)
PY
}

# ── 等待 TCP 端口就绪 ─────────────────────────────────────────────────────
wait_tcp() {
    local host="$1" port="$2" label="$3" timeout="${4:-10}"
    local end=$((SECONDS + timeout))
    while (( SECONDS < end )); do
        python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.3); r=s.connect_ex(('$host',$port)); s.close(); sys.exit(0 if r==0 else 1)" 2>/dev/null && return 0
        sleep 0.2
    done
    die "$label 在 ${timeout}s 内未就绪"
}

die() { log "ERROR: $*"; exit 2; }

# ── 启动 mock 上游 ────────────────────────────────────────────────────────
start_mock() {
    python3 "$MOCK_PY" "$MOCK_PORT" >"$run_dir/mock.log" 2>&1 &
    mock_pid=$!
    wait_tcp 127.0.0.1 "$MOCK_PORT" "mock upstream"
    log "mock upstream ready (pid=$mock_pid port=$MOCK_PORT)"
}

# ── 启动 toxiproxy(代理 mock,管理口随机端口) ─────────────────────────────
start_toxiproxy() {
    "$TOXIPROXY_SERVER" -host 127.0.0.1 -port "$TOXI_API_PORT" \
        >"$run_dir/toxi.log" 2>&1 &
    toxi_pid=$!
    wait_tcp 127.0.0.1 "$TOXI_API_PORT" "toxiproxy-server"
    # 创建代理:toxi_proxy_port → mock_port
    "$TOXIPROXY_CLI" -host "$TOXI_API" create \
        --listen "127.0.0.1:${TOXI_PROXY_PORT}" \
        --upstream "127.0.0.1:${MOCK_PORT}" \
        upstream_proxy
    log "toxiproxy ready (pid=$toxi_pid proxy=$TOXI_PROXY_PORT api=$TOXI_API_PORT)"
}

# ── 启动网关 ──────────────────────────────────────────────────────────────
start_gateway() {
    [[ -f "$run_dir/bin/gw" ]] || die "网关二进制不存在: $GW_BIN (用 LF_GW_BIN= 指定)"
    # anchorRoot 读 /proc/self/exe → run_dir/bin/gw → chdir run_dir
    # → 读 run_dir/api_gw/bin/gateway.yml  (prepare_runtime 已写好)
    "$run_dir/bin/gw" >"$run_dir/gw.log" 2>&1 &
    gw_pid=$!
    wait_tcp 127.0.0.1 "$GW_PORT" "gateway" 15
    local hc
    hc=$(curl -sf "http://127.0.0.1:${GW_ADMIN}/healthz" 2>/dev/null) || die "gateway healthz 失败"
    log "gateway ready (pid=$gw_pid port=$GW_PORT) — healthz: $hc"
}

# ── vegeta 发压:固定 target,报告保存到 out_prefix.bin + .txt ─────────────
run_vegeta() {
    local label="$1" rps="$2" dur="$3" token="$4"
    local out="$run_dir/${label}"
    log "vegeta[$label] 开始 — ${rps}rps × ${dur}s"
    printf 'GET http://127.0.0.1:%s/public/users\nAuthorization: Bearer %s\n\n' \
        "$GW_PORT" "$token" \
    | "$VEGETA_BIN" attack \
        -rate="${rps}" \
        -duration="${dur}s" \
        -timeout=8s \
        -max-connections=512 \
        -keepalive=true \
    > "${out}.bin"
    "$VEGETA_BIN" report -type=text  < "${out}.bin" > "${out}.txt"
    "$VEGETA_BIN" report -type=hdrplot < "${out}.bin" > "${out}.hdr" 2>/dev/null || true
    log "vegeta[$label] 完成 — 结果: ${out}.txt"
    cat "${out}.txt" >&2
}

# ── toxiproxy 毒素操作 ────────────────────────────────────────────────────
toxi_add() {
    # add <name> <type> [--attribute k=v ...]  作用到 upstream_proxy
    "$TOXIPROXY_CLI" -host "$TOXI_API" toxic add -n "$1" -t "$2" \
        "${@:3}" upstream_proxy
}
toxi_del() {
    "$TOXIPROXY_CLI" -host "$TOXI_API" toxic delete -n "$1" upstream_proxy 2>/dev/null || true
}

# ── 从 /stats 拿关键计数快照 ─────────────────────────────────────────────
gw_stats_cb() {
    curl -sf "http://127.0.0.1:${GW_ADMIN}/stats" 2>/dev/null \
    | python3 -c "
import sys, json
d = json.load(sys.stdin)
print('stats: circuit_open={circuit_open} upstream_fail={upstream_fail} '
      'no_healthy={no_healthy_endpoint} rate_limited={rate_limited} '
      '5xx={v5xx}'.format(
    circuit_open=d.get('circuit_open',0),
    upstream_fail=d.get('upstream_fail',0),
    no_healthy_endpoint=d.get('no_healthy_endpoint',0),
    rate_limited=d.get('rate_limited',0),
    v5xx=d.get('5xx',0)))
" 2>/dev/null \
    || echo "stats: (unavailable)"
}

# ══════════════════════════════════════════════════════════════════════════════
# main
# ══════════════════════════════════════════════════════════════════════════════
log "=== 阶段二:开环压测 + toxiproxy 故障注入 ==="
log "拓扑: vegeta → gw:$GW_PORT → toxiproxy:$TOXI_PROXY_PORT → mock:$MOCK_PORT"

# 准备
prepare_runtime
write_gw_config
start_mock
start_toxiproxy
start_gateway

# JWT(无过期问题:有效期 1 小时)
TOKEN=$(python3 "$GEN_JWT" valid "$JWT_SECRET" 2>/dev/null) || TOKEN=""
[[ -n "$TOKEN" ]] || die "JWT 生成失败"
log "JWT 已生成"

# ── 场景 1: baseline ──────────────────────────────────────────────────────
log ""; log "▶ 场景 1/4: baseline (${BASELINE_RPS}rps × ${BASELINE_SECS}s,无故障)"
run_vegeta "baseline" "$BASELINE_RPS" "$BASELINE_SECS" "$TOKEN"
gw_stats_cb

# ── 场景 2: 上游注入 +300ms 延迟(触发慢熔断) ─────────────────────────────
log ""; log "▶ 场景 2/4: latency_inject (+300ms 上游延迟,${FAULT_SECS}s)"
log "  熔断配置: slow_ms=1000 — 延迟 300ms 不触发慢熔断但压 p99;改成 1500ms 会触发"
toxi_add "latency1" "latency" --attribute "latency=1500" --attribute "jitter=100"
run_vegeta "latency_inject" "$FAULT_RPS" "$FAULT_SECS" "$TOKEN"
gw_stats_cb
toxi_del "latency1"
log "  [等待熔断恢复 ${RECOVERY_SECS}s]"; sleep "$RECOVERY_SECS"
gw_stats_cb

# ── 场景 3: 上游完全切断(熔断 OPEN → HALF_OPEN → CLOSED 全周期) ──────────
log ""; log "▶ 场景 3/4: upstream_down (切断上游 ${FAULT_SECS}s,观察全周期)"
toxi_add "timeout1" "timeout" --attribute "timeout=0"  # timeout=0 = 立刻切断
run_vegeta "upstream_down" "$FAULT_RPS" "$FAULT_SECS" "$TOKEN"
gw_stats_cb
toxi_del "timeout1"
log "  [等待半开探测恢复 ${RECOVERY_SECS}s]"; sleep "$RECOVERY_SECS"
gw_stats_cb

# ── 场景 4: 带宽限速 1KB/s(连接池驱逐 + 超时路径) ────────────────────────
log ""; log "▶ 场景 4/4: bandwidth_limit (上游限速 1KB/s,${FAULT_SECS}s)"
toxi_add "bw1" "bandwidth" --attribute "rate=1"
run_vegeta "bandwidth_limit" "$FAULT_RPS" "$FAULT_SECS" "$TOKEN"
gw_stats_cb
toxi_del "bw1"
log "  [恢复观察 ${RECOVERY_SECS}s]"; sleep "$RECOVERY_SECS"
run_vegeta "post_recovery" "$BASELINE_RPS" "15" "$TOKEN"

# ── 汇总 ──────────────────────────────────────────────────────────────────
log ""
log "══════════════════════ 结果汇总 ══════════════════════"
for f in "$run_dir"/*.txt; do
    label="$(basename "$f" .txt)"
    printf '[%s]\n' "$label" >&2
    cat "$f" >&2
    printf '\n' >&2
done
log "raw 数据(hdr): $run_dir/*.bin .hdr"
log "如需 HDR 直方图: vegeta report -type=hdrplot < baseline.bin | gnuplot ..."
# 检查 baseline 成功率
baseline_ok=$(python3 - "$run_dir/baseline.bin" <<'PY'
import sys
try:
    import subprocess, json
    r = subprocess.run(["vegeta", "report", "-type=json"],
        stdin=open(sys.argv[1],"rb"), capture_output=True)
    d = json.loads(r.stdout)
    rate = d.get("success", 0)
    print(f"baseline 成功率: {rate*100:.1f}%")
    sys.exit(0 if rate >= 0.99 else 1)
except Exception as e:
    print(f"(无法解析: {e})"); sys.exit(0)
PY
) && log "$baseline_ok" || { fail "baseline 成功率 < 99%"; log "$baseline_ok"; }
