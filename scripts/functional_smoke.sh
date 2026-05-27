#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/scripts/lib/bench_common.sh"

OUT_DIR="${OUT_DIR:-bench_output/smoke}"
mkdir -p "$OUT_DIR"
RESULT="$OUT_DIR/smoke_result.txt"

{
  echo "=== functional smoke $(date -Iseconds) ==="
  echo "[1] wss_unit_tests"
  ./build/wss_unit_tests && echo "  PASS unit_tests" || echo "  FAIL unit_tests"

  echo "[2] gen_cert"
  bench_check_certs && echo "  PASS certs" || echo "  FAIL certs"

  echo "[3] handshake-only"
  LOG="$OUT_DIR/server_handshake.log"
  bench_start_server "$LOG" --port 18443 --cert certs/server_cert.pem --key certs/server_key.pem --http-port 0 --no-log-file
  if ./build/wss_client --host 127.0.0.1 --port 18443 --ca certs/server_cert.pem --handshake-only --quiet --no-log-file | grep -q 'WSS_BENCH ok'; then
    echo "  PASS handshake-only"
  else
    echo "  FAIL handshake-only"
  fi
  bench_stop_server

  echo "[4] text echo (run_demo)"
  if ./scripts/run_demo.sh >/dev/null 2>&1; then
    echo "  PASS run_demo"
  else
    echo "  FAIL run_demo"
  fi

  echo "[5] selftest_0rtt"
  if timeout 45 ./scripts/selftest_0rtt.sh >/dev/null 2>&1; then
    echo "  PASS selftest_0rtt"
  else
    echo "  FAIL selftest_0rtt"
  fi
} | tee "$RESULT"

if grep -q 'FAIL' "$RESULT"; then
  echo "[smoke] 存在失败项" >&2
  exit 1
fi
echo "[smoke] 全部通过 -> $RESULT"
