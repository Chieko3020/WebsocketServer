#!/usr/bin/env bash
# 双终端 0-RTT 自测说明与一键顺序演示（模拟「终端 A 仅服务端 / 终端 B 仅客户端」）
#
# 手动方式（推荐对照日志）：
#   终端 A:  cd /path/to/WebsocketServer
#            ./build/wss_server --enable-0rtt 1 --port 8443 \
#              --cert certs/server_cert.pem --key certs/server_key.pem \
#              --http-port 8080 --http-root web
#   终端 B 第一次:
#            rm -f /tmp/wss_0rtt_sess.pem
#            ./build/wss_client --host 127.0.0.1 --port 8443 --ca certs/server_cert.pem \
#              --text "hello" --enable-0rtt 1 --session-file /tmp/wss_0rtt_sess.pem
#   终端 B 第二次（同命令再执行一次）:
#            ./build/wss_client --host 127.0.0.1 --port 8443 --ca certs/server_cert.pem \
#              --text "hello2" --enable-0rtt 1 --session-file /tmp/wss_0rtt_sess.pem
#
# 期望（第二次）：客户端 try_SSL_write_early_data=1、early_data_status 可为 ACCEPTED、
#               TLS_session_reused=1；服务端 session_reused=1。

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
PORT="${WSS_TEST_PORT:-18555}"
SESS="${WSS_TEST_SESS:-/tmp/wss_0rtt_sess.pem}"

cmake --build build -j >/dev/null
rm -f "$SESS"

echo "========== 终端 A（本脚本内子进程，日志带 [A] 前缀；真实自测请另开终端只跑 wss_server）=========="
# stdbuf -oL 行缓冲，避免 sed 侧日志与客户端交错混乱
stdbuf -oL ./build/wss_server --enable-0rtt 1 --port "$PORT" \
  --cert certs/server_cert.pem --key certs/server_key.pem \
  --http-port 0 2>&1 | stdbuf -oL sed 's/^/[A] /' &
SRV_PID=$!
cleanup() { kill "$SRV_PID" 2>/dev/null || true; wait "$SRV_PID" 2>/dev/null || true; }
trap cleanup EXIT
sleep 2

echo ""
echo "========== 终端 B — 第 1 次客户端 =========="
./build/wss_client --host 127.0.0.1 --port "$PORT" --ca certs/server_cert.pem \
  --text "t1" --enable-0rtt 1 --session-file "$SESS" 2>&1 | sed 's/^/[B1] /' || true

echo ""
echo "========== 终端 B — 第 2 次客户端 =========="
./build/wss_client --host 127.0.0.1 --port "$PORT" --ca certs/server_cert.pem \
  --text "t2" --enable-0rtt 1 --session-file "$SESS" 2>&1 | sed 's/^/[B2] /' || true

echo ""
echo "========== 会话 PEM 摘要 =========="
openssl sess_id -in "$SESS" -inform PEM -text 2>/dev/null | grep -E 'Max Early|Timeout' || true
