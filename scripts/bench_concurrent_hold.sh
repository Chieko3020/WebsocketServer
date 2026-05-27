#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/scripts/lib/bench_common.sh"

PORT="${PORT:-8450}"
HOST="${HOST:-127.0.0.1}"
CA_FILE="${CA_FILE:-certs/server_cert.pem}"
CERT_FILE="${CERT_FILE:-certs/server_cert.pem}"
KEY_FILE="${KEY_FILE:-certs/server_key.pem}"
HOLD_SECONDS="${HOLD_SECONDS:-30}"
OUT_DIR="${OUT_DIR:-bench_output/concurrent_hold}"
# 默认分档；SKIP_HEAVY=1 时仅去掉 1000，保留外部已设置的 HOLD_TARGETS（如 QUICK 的 50 100）
if [[ -z "${HOLD_TARGETS:-}" ]]; then
  HOLD_TARGETS="200 500 1000"
fi
if [[ "${SKIP_HEAVY:-0}" == "1" ]]; then
  HOLD_TARGETS="$(echo "$HOLD_TARGETS" | tr ' ' '\n' | grep -v '^1000$' | tr '\n' ' ' | sed 's/ $//')"
  [[ -z "$HOLD_TARGETS" ]] && HOLD_TARGETS="200 500"
fi

mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/concurrent_hold.csv"
echo "target,success,fail,success_rate_pct,cpu_peak_pct,rss_peak_kb,ulimit_n" >"$CSV"

LOG="$OUT_DIR/server.log"
bench_check_ulimit 1000

bench_start_server "$LOG" \
  --port "$PORT" --cert "$CERT_FILE" --key "$KEY_FILE" \
  --http-port 0 --ws-idle-timeout 300 --ws-ping-interval 30 \
  --no-log-file || exit 1
trap 'bench_stop_server' EXIT

for N in $HOLD_TARGETS; do
  bench_check_ulimit "$N"
  echo "[bench] 并发档 N=$N hold=${HOLD_SECONDS}s ..."
  ok=0
  fail=0
  cpu_peak=0
  rss_peak=0
  pids=()
  for ((i=1; i<=N; i++)); do
    ./build/wss_client --host "$HOST" --port "$PORT" --ca "$CA_FILE" \
      --handshake-only --hold-seconds "$HOLD_SECONDS" --no-log-file --quiet \
      >/dev/null 2>&1 &
    pids+=($!)
    if (( i % 50 == 0 )); then
      if read -r cpu rss _ < <(ps -p "$BENCH_SERVER_PID" -o %cpu=,rss= 2>/dev/null || echo "0 0"); then
        cpu_int="${cpu%.*}"
        [[ -z "$cpu_int" ]] && cpu_int=0
        (( cpu_int > cpu_peak )) && cpu_peak=$cpu_int
        (( rss > rss_peak )) && rss_peak=$rss
      fi
    fi
  done
  for pid in "${pids[@]}"; do
    if wait "$pid"; then
      ((ok++)) || true
    else
      ((fail++)) || true
    fi
  done
  if read -r cpu rss _ < <(ps -p "$BENCH_SERVER_PID" -o %cpu=,rss= 2>/dev/null || echo "0 0"); then
    cpu_int="${cpu%.*}"
    [[ -z "$cpu_int" ]] && cpu_int=0
    (( cpu_int > cpu_peak )) && cpu_peak=$cpu_int
    (( rss > rss_peak )) && rss_peak=$rss
  fi
  total=$((ok + fail))
  rate=0
  if [[ "$total" -gt 0 ]]; then
    rate=$(awk "BEGIN {printf \"%.2f\", 100.0*$ok/$total}")
  fi
  echo "$N,$ok,$fail,$rate,$cpu_peak,$rss_peak,$(ulimit -n)" >>"$CSV"
  echo "[bench] N=$N 成功=$ok 失败=$fail 成功率=${rate}% CPU峰值≈${cpu_peak}% RSS峰值≈${rss_peak}KB"
done

python3 - "$CSV" "$OUT_DIR/concurrent_hold_summary.md" <<'PY'
import csv, sys
csv_path, out_md = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(csv_path, newline='', encoding='utf-8')))
with open(out_md, 'w', encoding='utf-8') as o:
    o.write('# 同时在线并发压测\n\n')
    o.write(f'- hold_seconds: {__import__("os").environ.get("HOLD_SECONDS", "15")}\n')
    o.write(f'- ulimit_n: {__import__("resource").getrlimit(__import__("resource").RLIMIT_NOFILE)[0] if hasattr(__import__("resource"), "RLIMIT_NOFILE") else "n/a"}\n\n')
    o.write('| 目标连接数 | 成功 | 失败 | 成功率% | CPU峰值% | RSS峰值KB |\n')
    o.write('|------------|------|------|---------|----------|----------|\n')
    stable_max = 0
    for r in rows:
        o.write(f'| {r["target"]} | {r["success"]} | {r["fail"]} | {r["success_rate_pct"]} | {r["cpu_peak_pct"]} | {r["rss_peak_kb"]} |\n')
        if float(r['success_rate_pct']) >= 95.0:
            stable_max = max(stable_max, int(r['target']))
    o.write(f'\n**实测稳定上限（成功率≥95%）**: {stable_max or "见上表"}\n')
    o.write('\n开题指标参考：1000+ 同时在线；若未达标请引用本表实际上限。\n')
PY

echo "[bench] 汇总: $OUT_DIR/concurrent_hold_summary.md"
