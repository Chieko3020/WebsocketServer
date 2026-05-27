#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/scripts/lib/bench_common.sh"
source "$ROOT/scripts/lib/parse_bench_line.sh"

PORT="${PORT:-8451}"
HOST="${HOST:-127.0.0.1}"
CA_FILE="${CA_FILE:-certs/server_cert.pem}"
CERT_FILE="${CERT_FILE:-certs/server_cert.pem}"
KEY_FILE="${KEY_FILE:-certs/server_key.pem}"
ROUNDS="${ROUNDS:-50}"
OUT_DIR="${OUT_DIR:-bench_output/session_reuse}"
SESS_FILE="${SESS_FILE:-$OUT_DIR/session.pem}"

mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/server.log"
CSV="$OUT_DIR/session_reuse.csv"
echo "round,handshake_ms,session_reused_client,success" >"$CSV"

bench_start_server "$LOG" \
  --port "$PORT" --cert "$CERT_FILE" --key "$KEY_FILE" \
  --http-port 0 --enable-ticket 1 --no-log-file
trap 'bench_stop_server' EXIT

rm -f "$SESS_FILE"

for ((r=1; r<=ROUNDS; r++)); do
  out=$(./build/wss_client --host "$HOST" --port "$PORT" --ca "$CA_FILE" \
    --handshake-only --quiet --no-log-file \
    --session-file "$SESS_FILE" --enable-ticket 1 2>/dev/null | tail -n1) || true
  if parse_wss_bench_line "$out"; then
    echo "$r,$BENCH_HANDSHAKE_MS,$BENCH_SESSION_REUSED,1" >>"$CSV"
  else
    echo "$r,0,0,0" >>"$CSV"
  fi
done

sleep 3
read -r hs_ok hs_fail hs_new hs_reused < <(bench_parse_server_metrics "$LOG")

mapfile -t _bench_lines < <(python3 - "$CSV" "$OUT_DIR/session_reuse_summary.md" "$hs_reused" "$hs_new" <<'PY'
import csv, statistics, sys
csv_path, out_md, srv_reused, srv_new = sys.argv[1:5]
vals = []
reused_cli = 0
tot = 0
after_first = 0
reused_after = 0
with open(csv_path, newline='', encoding='utf-8') as f:
    for i, row in enumerate(csv.DictReader(f)):
        tot += 1
        if row['success'] == '1':
            vals.append(float(row['handshake_ms']))
            if row['session_reused_client'] == '1':
                reused_cli += 1
            if i > 0:
                after_first += 1
                if row['session_reused_client'] == '1':
                    reused_after += 1
client_rate = (100.0 * reused_after / after_first) if after_first > 0 else 0.0
srv_denom = int(srv_reused) + int(srv_new)
srv_rate = (100.0 * int(srv_reused) / srv_denom) if srv_denom > 0 else 0.0
rate = max(client_rate, srv_rate)
p = "PASS" if rate >= 80.0 else "FAIL"
avg = statistics.mean(vals) if vals else 0
with open(out_md, 'w', encoding='utf-8') as o:
    o.write('# 会话复用压测\n\n')
    o.write(f'- 轮次: {tot}\n')
    o.write(f'- 客户端 session_reused（第2轮起）: {reused_after}/{after_first} = **{client_rate:.1f}%**\n')
    o.write(f'- 服务端日志 复用握手: {srv_reused}, 新握手: {srv_new} = **{srv_rate:.1f}%**\n')
    o.write(f'- **综合复用率**: **{rate:.1f}%** — 开题≥80%: **{p}**\n')
    o.write(f'- 平均 handshake_ms (成功): {avg:.1f}\n')
print(f"{rate:.2f}")
print(p)
PY
)
reuse_rate="${_bench_lines[0]:-0}"
pass="${_bench_lines[1]:-?}"

echo "[bench] 会话复用率=${reuse_rate}% (${pass}) -> $OUT_DIR/session_reuse_summary.md"
