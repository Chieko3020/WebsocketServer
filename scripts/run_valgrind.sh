#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/scripts/lib/bench_common.sh"

PORT="${PORT:-8456}"
CERT_FILE="${CERT_FILE:-certs/server_cert.pem}"
KEY_FILE="${KEY_FILE:-certs/server_key.pem}"
CA_FILE="${CA_FILE:-certs/server_cert.pem}"
OUT_DIR="${OUT_DIR:-bench_output/valgrind}"
mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/valgrind.log"

bench_check_certs || exit 1

if ! command -v valgrind >/dev/null 2>&1; then
  echo "[bench] valgrind 未安装，跳过" >&2
  echo "SKIP: valgrind not installed" >"$OUT_DIR/valgrind_summary.md"
  exit 0
fi

valgrind --leak-check=full --error-exitcode=1 \
  ./build/wss_server --port "$PORT" --cert "$CERT_FILE" --key "$KEY_FILE" \
  --http-port 0 --no-log-file >"$OUT_DIR/server_stdout.log" 2>"$LOG" &
VG_PID=$!
sleep 2
trap 'kill $VG_PID 2>/dev/null; wait $VG_PID 2>/dev/null || true' EXIT

for ((i=1; i<=10; i++)); do
  ./build/wss_client --host 127.0.0.1 --port "$PORT" --ca "$CA_FILE" \
    --handshake-only --quiet --no-log-file >/dev/null 2>&1 || true
done

kill "$VG_PID" 2>/dev/null || true
wait "$VG_PID" 2>/dev/null || VG_EXIT=$?
VG_EXIT=${VG_EXIT:-0}

definitely=$(grep -oP 'definitely lost: \K[0-9,]+' "$LOG" 2>/dev/null | tail -n1 | tr -d ',' || echo "0")
errors=$(grep "ERROR SUMMARY" "$LOG" 2>/dev/null | tail -n1 || echo "n/a")

{
  echo "# Valgrind 检测"
  echo ""
  echo "- ERROR SUMMARY: $errors"
  echo "- definitely lost (bytes): ${definitely:-unknown}"
  echo ""
  if [[ "$definitely" == "0" ]] && [[ "$VG_EXIT" -eq 0 || "$VG_EXIT" -eq 143 ]]; then
    echo "**结论**: PASS（无 definitely lost；OpenSSL 内部 reachable 可忽略）"
  else
    echo "**结论**: 请人工查看 $LOG"
  fi
} >"$OUT_DIR/valgrind_summary.md"

echo "[bench] -> $OUT_DIR/valgrind_summary.md"
