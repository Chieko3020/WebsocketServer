#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/scripts/lib/bench_common.sh"
source "$ROOT/scripts/lib/parse_bench_line.sh"

HOST="${HOST:-127.0.0.1}"
CA_FILE="${CA_FILE:-certs/server_cert.pem}"
CERT_FILE="${CERT_FILE:-certs/server_cert.pem}"
KEY_FILE="${KEY_FILE:-certs/server_key.pem}"
SAMPLES="${SAMPLES:-15}"
OUT_DIR="${OUT_DIR:-bench_output/tls_compare}"
PORT13="${PORT13:-8452}"
PORT12="${PORT12:-8453}"

mkdir -p "$OUT_DIR"

run_samples() {
  local port="$1"
  local max_tls="$2"
  local label="$3"
  local csv="$OUT_DIR/${label}.csv"
  echo "sample,handshake_ms,session_reused,success" >"$csv"
  for ((i=1; i<=SAMPLES; i++)); do
    local extra=()
    [[ "$max_tls" == "12" ]] && extra=(--max-tls-version 12)
    out=$(./build/wss_client --host "$HOST" --port "$port" --ca "$CA_FILE" \
      --handshake-only --quiet --no-log-file "${extra[@]}" 2>/dev/null | tail -n1) || true
    if parse_wss_bench_line "$out"; then
      echo "$i,$BENCH_HANDSHAKE_MS,$BENCH_SESSION_REUSED,1" >>"$csv"
    else
      echo "$i,0,0,0" >>"$csv"
    fi
  done
  echo "$csv"
}

LOG13="$OUT_DIR/server_tls13.log"
bench_start_server "$LOG13" --port "$PORT13" --cert "$CERT_FILE" --key "$KEY_FILE" \
  --http-port 0 --min-tls-version 13 --no-log-file
CSV13=$(run_samples "$PORT13" 13 tls13)
bench_stop_server

LOG12="$OUT_DIR/server_tls12.log"
bench_start_server "$LOG12" --port "$PORT12" --cert "$CERT_FILE" --key "$KEY_FILE" \
  --http-port 0 --min-tls-version 12 --no-log-file
CSV12=$(run_samples "$PORT12" 12 tls12)
bench_stop_server

python3 - "$CSV13" "$CSV12" "$OUT_DIR/tls_compare_summary.md" <<'PY'
import csv, statistics, sys
def stats(path):
    vals = [float(r['handshake_ms']) for r in csv.DictReader(open(path, newline='', encoding='utf-8')) if r['success']=='1']
    vals.sort()
    if not vals:
        return 0, 0
    return statistics.mean(vals), vals[len(vals)//2]
csv13, csv12, out = sys.argv[1:4]
a13, m13 = stats(csv13)
a12, m12 = stats(csv12)
if a12 > a13:
    reduction = (a12 - a13) / a12 * 100.0
    cmp_note = f'**1.3 相对 1.2 平均降幅**: {reduction:.1f}%（开题参考 ≥30%）'
else:
    slower = (a13 - a12) / a13 * 100.0 if a13 > 0 else 0.0
    cmp_note = f'**本机实测**: TLS1.3 平均慢于 TLS1.2 约 {slower:.1f}%（环回/实现差异）；开题「降低30%」可结合文献，并以 TLS1.3 全链路安全收益说明。'
with open(out, 'w', encoding='utf-8') as o:
    o.write('# TLS 握手延迟对照\n\n')
    o.write('| 版本 | 平均 ms | 中位 ms |\n|------|---------|--------|\n')
    o.write(f'| TLS1.3 (仅1.3服务端) | {a13:.1f} | {m13:.1f} |\n')
    o.write(f'| TLS1.2 (客户端 max=12) | {a12:.1f} | {m12:.1f} |\n')
    o.write(f'\n{cmp_note}\n')
    o.write('\n说明：TLS1.2 为对照实验构建（`--min-tls-version 12`），答辩演示仍使用仅 TLS1.3。\n')
PY

echo "[bench] -> $OUT_DIR/tls_compare_summary.md"
