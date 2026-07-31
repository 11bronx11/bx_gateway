#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
DURATION="${1:-30s}"
RATE="${BRONX_DEMO_RATE:-50}"
TARGET="${BRONX_DEMO_TARGET:-https://gw.local/api/get}"
CA_FILE="$ROOT_DIR/demo/nginx/ca/ca.crt"

if ! command -v vegeta >/dev/null 2>&1; then
    echo "vegeta is required for the open-loop demo load" >&2
    exit 1
fi
if [[ ! -f "$CA_FILE" ]]; then
    echo "missing demo CA: $CA_FILE" >&2
    exit 1
fi

echo "load: target=$TARGET rate=$RATE/s duration=$DURATION"
printf 'GET %s\n' "$TARGET" |
    vegeta attack \
        -duration="$DURATION" \
        -rate="$RATE" \
        -root-certs="$CA_FILE" \
        -connect-to='gw.local:443:127.0.0.1:443' \
        -timeout=10s |
    vegeta report
