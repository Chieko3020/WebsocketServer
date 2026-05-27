#!/usr/bin/env bash
# 0-RTT 实验自测：默认配置下服务端开启 enable_0rtt，客户端默认带 X-Nonce；
# 若握手失败日志中出现 Missing X-Nonce，则本脚本以非零退出。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT="${PORT:-9443}"
HTTP_PORT="${HTTP_PORT:-9080}"
CERT="${CERT:-certs/server_cert.pem}"
KEY="${KEY:-certs/server_key.pem}"
CA="${CA:-certs/server_cert.pem}"

if [[ ! -f "$CERT" || ! -f "$KEY" ]]; then
  echo "缺少证书，请先运行: ./scripts/gen_cert.sh certs"
  exit 2
fi

OUT="/tmp/wss_selftest_0rtt.$$"
./build/wss_server --port "$PORT" --cert "$CERT" --key "$KEY" \
  --http-port 0 --enable-0rtt 1 --no-log-file >"$OUT" 2>&1 &
PID=$!

cleanup() {
  kill "$PID" >/dev/null 2>&1 || true
  wait "$PID" >/dev/null 2>&1 || true
  rm -f "$OUT"
}
trap cleanup EXIT

sleep 1

# 默认 enable0Rtt=1，升级请求含 X-Nonce
if ! ./build/wss_client --host 127.0.0.1 --port "$PORT" --ca "$CA" --text "0rtt-selftest" \
  --enable-0rtt 1 --client-ping-interval 10 >>"$OUT" 2>&1; then
  echo "wss_client 失败，日志:"
  tail -80 "$OUT"
  exit 1
fi

if grep -q "Missing X-Nonce" "$OUT"; then
  echo "失败：服务端报告 Missing X-Nonce"
  grep -n "X-Nonce\|0-RTT\|WebSocket" "$OUT" | tail -30
  exit 1
fi

if grep -q "帧解析失败\|SSL_connect failed\|Handshake failed" "$OUT"; then
  echo "失败：握手或解析错误"
  tail -40 "$OUT"
  exit 1
fi

echo "selftest_0rtt: OK"
