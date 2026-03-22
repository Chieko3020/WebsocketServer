#!/usr/bin/env bash
# 文件作用说明：
# 一键演示脚本：启动服务端 -> 运行客户端（文本+文件）-> 自动清理服务进程。
set -euo pipefail

# 允许通过环境变量覆盖默认参数，便于快速做不同场景验证。
PORT="${PORT:-8443}"
HOST="${HOST:-127.0.0.1}"
CA_FILE="${CA_FILE:-certs/server_cert.pem}"
CERT_FILE="${CERT_FILE:-certs/server_cert.pem}"
KEY_FILE="${KEY_FILE:-certs/server_key.pem}"

CHUNK_SIZE="${CHUNK_SIZE:-16384}"
TEXT="${TEXT:-hello}"

FILE="${FILE:-test_upload.bin}"
if [[ ! -f "$FILE" ]]; then
  echo "Creating demo file: $FILE"
  # 生成 256 KiB 随机测试文件，作为上传样本。
  dd if=/dev/urandom of="$FILE" bs=1024 count=256 status=none
fi

echo "Starting server (TLS1.3 + WebSocket)..."
# 后台启动服务端，并把日志写入文件便于问题复盘。
./build/wss_server --port "$PORT" --cert "$CERT_FILE" --key "$KEY_FILE" > server_demo.log 2>&1 &
SERVER_PID=$!

cleanup() {
  # 无论脚本正常结束或异常退出，都尽量回收后台服务进程。
  kill "$SERVER_PID" >/dev/null 2>&1 || true
  wait "$SERVER_PID" >/dev/null 2>&1 || true
}
trap cleanup EXIT

sleep 1

echo "浏览器演示（内置 HTTP，无需 python）：http://${HOST}:8080/ 或 http://${HOST}:8080/index.html"

echo "Running client (text echo + file upload/DR resume)..."
# 客户端执行文本回显 + 文件分片上传（含续传协议路径）。
./build/wss_client --host "$HOST" --port "$PORT" --ca "$CA_FILE" \
  --text "$TEXT" \
  --file "$FILE" \
  --chunk-size "$CHUNK_SIZE"

echo "Demo finished. Server log: server_demo.log"

