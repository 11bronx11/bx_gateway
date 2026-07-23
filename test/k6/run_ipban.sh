#!/usr/bin/env bash
# IP 封禁 k6 测试包装脚本
# 用法：bash test/k6/run_ipban.sh [ban_ttl_ms]
# 默认封禁 TTL = 15000ms（15s），方便测试自动解封
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BANCTL="${BANCTL:-$REPO/bin/banctl}"
K6="${K6:-k6}"
SCRIPT="$REPO/test/k6/scenarios/09_ipban.js"

BAN_TTL_MS="${1:-15000}"
TARGET_IP="${K6_IPBAN_TEST_IP:-127.0.0.1}"
SUMMARY_DIR="${K6_SUMMARY_DIR:-}"

preban_summary=()
recovery_summary=()
if [[ -n "$SUMMARY_DIR" ]]; then
  mkdir -p "$SUMMARY_DIR"
  preban_summary=(--summary-export "$SUMMARY_DIR/prebanned-summary.json")
  recovery_summary=(--summary-export "$SUMMARY_DIR/recovery-summary.json")
fi

# hub admin socket（与 hub.yml 中 admin_sock 对应）
HUB_ADMIN="${HUB_ADMIN_SOCK:-/tmp/bronx_ip_admin.sock}"

if [[ ! -x "$BANCTL" ]]; then
  echo "ERROR: banctl not found at $BANCTL" >&2
  exit 1
fi

cleanup() {
  echo "--- cleanup: unbanning $TARGET_IP ---"
  "$BANCTL" --sock "$HUB_ADMIN" unban "$TARGET_IP" 2>/dev/null || true
}
trap cleanup EXIT

echo "--- phase 1: ban $TARGET_IP for ${BAN_TTL_MS}ms ---"
"$BANCTL" --sock "$HUB_ADMIN" ban "$TARGET_IP/32" --ttl "${BAN_TTL_MS}ms" --reason "k6-test"
sleep 1   # 等快照传播到网关

echo "--- phase 2: k6 verifies 403 while banned ---"
K6_PREBANNED=1 K6_BAN_MS="$BAN_TTL_MS" \
  K6_IPBAN_TEST_IP="$TARGET_IP" \
  "$K6" run "${preban_summary[@]}" "$SCRIPT" \
  -e K6_GW_HOST="${K6_GW_HOST:-127.0.0.1}" \
  -e K6_GW_PORT="${K6_GW_PORT:-8090}" \
  -e K6_HUB_HOST="${K6_HUB_HOST:-127.0.0.1}" \
  -e K6_HUB_PORT="${K6_HUB_PORT:-9091}" \
  -e K6_IPBAN_TEST_IP="$TARGET_IP" \
  "$@"

echo "--- phase 3: wait for ban expiry (${BAN_TTL_MS}ms) ---"
sleep $((BAN_TTL_MS / 1000 + 2))

echo "--- phase 4: k6 verifies recovery ---"
K6_PREBANNED=0 K6_IPBAN_RECOVERY=1 \
  K6_IPBAN_TEST_IP="$TARGET_IP" \
  "$K6" run "${recovery_summary[@]}" "$SCRIPT" \
  -e K6_GW_HOST="${K6_GW_HOST:-127.0.0.1}" \
  -e K6_GW_PORT="${K6_GW_PORT:-8090}" \
  -e K6_IPBAN_TEST_IP="$TARGET_IP" \
  -e K6_IPBAN_RECOVERY=1 \
  --scenario static_allow \
  "$@"

echo "--- ipban test DONE ---"
