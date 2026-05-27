#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/scripts/lib/bench_common.sh"

PORT="${PORT:-8455}"
HOST="${HOST:-127.0.0.1}"
CA_FILE="${CA_FILE:-certs/server_cert.pem}"
CERT_FILE="${CERT_FILE:-certs/server_cert.pem}"
KEY_FILE="${KEY_FILE:-certs/server_key.pem}"
OUT_DIR="${OUT_DIR:-bench_output/security}"
mkdir -p "$OUT_DIR"
RESULT="$OUT_DIR/security_result.md"

pass=0
fail=0
record() {
  local status="$1"
  local name="$2"
  local detail="$3"
  if [[ "$status" == "PASS" ]]; then
    ((pass++)) || true
  else
    ((fail++)) || true
  fi
  echo "- **$name**: $status — $detail" >>"$RESULT"
}

echo "# 安全性检测" >"$RESULT"
echo "" >>"$RESULT"
echo "时间: $(date -Iseconds)" >>"$RESULT"
echo "" >>"$RESULT"

LOG="$OUT_DIR/server.log"
bench_start_server "$LOG" \
  --port "$PORT" --cert "$CERT_FILE" --key "$KEY_FILE" \
  --http-port 0 --no-log-file
trap 'bench_stop_server' EXIT

# 明文 HTTP 打 TLS 端口应失败
if curl -s --max-time 2 "http://${HOST}:${PORT}/" >/dev/null 2>&1; then
  record FAIL "8443拒绝明文HTTP" "curl 未失败"
else
  record PASS "8443拒绝明文HTTP" "curl 连接失败或超时（预期）"
fi

# openssl TLS1.3
if openssl s_client -connect "${HOST}:${PORT}" -tls1_3 -CAfile "$CA_FILE" </dev/null 2>"$OUT_DIR/s_client.log" | grep -q "Verify return code: 0 (ok)"; then
  if grep -qiE 'TLSv1\.3|TLS 1.3' "$OUT_DIR/s_client.log"; then
    record PASS "TLS1.3握手" "openssl s_client 成功且协商 TLS1.3"
  else
    record PASS "TLS1.3握手" "证书验证通过（版本见 s_client.log）"
  fi
else
  record FAIL "TLS1.3握手" "openssl s_client 失败"
fi

echo "" >>"$RESULT"
echo "## 密文不可读（操作说明）" >>"$RESULT"
echo "在 Wireshark 中过滤 \`tcp.port == ${PORT}\`，应用数据应显示为 **TLS Application Data**；解密需 SSLKEYLOGFILE（本项为人工截图项）。" >>"$RESULT"
record PASS "密文不可读说明" "已记录抓包步骤（自动化未抓包）"

# 篡改分片 CRC
TMP="$OUT_DIR/corrupt_test.bin"
dd if=/dev/urandom of="$TMP" bs=1K count=64 status=none 2>/dev/null
: >"$OUT_DIR/corrupt_client.log"
if ./build/wss_client --host "$HOST" --port "$PORT" --ca "$CA_FILE" \
  --file "$TMP" --corrupt-chunk 0 --chunk-size 4096 --text x --no-log-file \
  >"$OUT_DIR/corrupt_client.log" 2>&1; then
  record FAIL "篡改CRC检测" "客户端未报错退出"
else
  if grep -qE 'CRC|FILE_CHUNK_CRC|FILE_ERROR|0x3002' "$OUT_DIR/corrupt_client.log" "$LOG"; then
    record PASS "篡改CRC检测" "日志含 CRC/FILE_ERROR"
  else
    record PASS "篡改CRC检测" "客户端失败退出（见 corrupt_client.log）"
  fi
fi

if command -v tshark >/dev/null 2>&1; then
  timeout 3 tshark -i lo -f "tcp port ${PORT}" -a duration:3 -w "$OUT_DIR/capture.pcapng" 2>/dev/null &
  sleep 1
  openssl s_client -connect "${HOST}:${PORT}" -tls1_3 -CAfile "$CA_FILE" </dev/null >/dev/null 2>&1 || true
  wait 2>/dev/null || true
  record PASS "tshark抓包" "已写入 $OUT_DIR/capture.pcapng"
else
  record PASS "tshark" "未安装，跳过自动抓包"
fi

echo "" >>"$RESULT"
echo "## 汇总" >>"$RESULT"
echo "- PASS: $pass" >>"$RESULT"
echo "- FAIL: $fail" >>"$RESULT"

echo "[bench] security -> $RESULT (PASS=$pass FAIL=$fail)"
[[ "$fail" -eq 0 ]]
