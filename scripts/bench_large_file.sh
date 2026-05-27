#!/usr/bin/env bash
set -euo pipefail

# 大文件分档上传压测。
# 轮次（按文件大小，未单独设置时回退到 ROUNDS）：
#   ROUNDS_10MB / ROUNDS_100MB / ROUNDS_1000MB
#   或 BENCH_LARGE_ROUNDS_10 / _100 / _1000（run_all 使用）
# 其它：SIZES_MB、BENCH_INCLUDE_1GB、SKIP_1GB、CHUNK_SIZE、CLIENT_PING

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/scripts/lib/bench_common.sh"

PORT="${PORT:-8454}"
HOST="${HOST:-127.0.0.1}"
CA_FILE="${CA_FILE:-certs/server_cert.pem}"
CERT_FILE="${CERT_FILE:-certs/server_cert.pem}"
KEY_FILE="${KEY_FILE:-certs/server_key.pem}"
ROUNDS="${ROUNDS:-3}"
OUT_DIR="${OUT_DIR:-bench_output/large_file}"
SIZES_MB="${SIZES_MB:-10 100}"
# 需要 1GB 档：export SIZES_MB="10 100 1000" 或 BENCH_INCLUDE_1GB=1（且未设 SKIP_1GB）
if [[ "${SKIP_1GB:-0}" != "1" && "${BENCH_INCLUDE_1GB:-0}" == "1" ]]; then
  SIZES_MB="${SIZES_MB} 1000"
fi

# 各档轮次：优先 ROUNDS_<N>MB，其次 BENCH_LARGE_ROUNDS_<N>，最后 ROUNDS
rounds_for_mb() {
  local mb="$1"
  local v=""
  case "$mb" in
    10) v="${ROUNDS_10MB:-${BENCH_LARGE_ROUNDS_10:-}}" ;;
    100) v="${ROUNDS_100MB:-${BENCH_LARGE_ROUNDS_100:-}}" ;;
    1000) v="${ROUNDS_1000MB:-${BENCH_LARGE_ROUNDS_1000:-}}" ;;
    *) v="" ;;
  esac
  if [[ -z "$v" ]]; then
    v="$ROUNDS"
  fi
  if [[ ! "$v" =~ ^[0-9]+$ ]] || [[ "$v" -lt 1 ]]; then
    echo "[bench] ERROR: ${mb}MB 轮次无效: $v" >&2
    exit 1
  fi
  echo "$v"
}

mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/large_file.csv"
echo "size_mb,round,elapsed_ms,success,crc_errors" >"$CSV"

LOG="$OUT_DIR/server.log"
bench_start_server "$LOG" \
  --port "$PORT" --cert "$CERT_FILE" --key "$KEY_FILE" \
  --http-port 0 --ws-idle-timeout 600 --ws-ping-interval 30 \
  --no-log-file
trap 'bench_stop_server' EXIT

CHUNK_SIZE="${CHUNK_SIZE:-32768}"
CLIENT_PING="${CLIENT_PING:-5}"

echo "[bench] 分档轮次配置（ROUNDS 默认=${ROUNDS}）："
for mb in $SIZES_MB; do
  echo "[bench]   ${mb}MB -> $(rounds_for_mb "$mb") 轮"
done

for mb in $SIZES_MB; do
  mb_rounds=$(rounds_for_mb "$mb")
  TMP="$OUT_DIR/test_${mb}mb.bin"
  if [[ ! -f "$TMP" ]]; then
    echo "[bench] 生成 ${mb}MB 测试文件..."
    dd if=/dev/urandom of="$TMP" bs=1M count="$mb" status=none
  fi
  for ((r=1; r<=mb_rounds; r++)); do
    echo "[bench] 上传 ${mb}MB round=$r ..."
    start=$(date +%s%3N)
    client_log="$OUT_DIR/client_${mb}mb_r${r}.log"
    if ./build/wss_client --host "$HOST" --port "$PORT" --ca "$CA_FILE" \
      --file "$TMP" --chunk-size "$CHUNK_SIZE" --text "bench" \
      --client-ping-interval "$CLIENT_PING" --no-log-file \
      >"$client_log" 2>&1; then
      end=$(date +%s%3N)
      elapsed=$((end - start))
      crc_err=$(grep -hE 'FILE_CHUNK_CRC_MISMATCH|FILE_ERROR' "$client_log" "$LOG" 2>/dev/null | wc -l | tr -d ' ' || true)
      [[ -z "$crc_err" ]] && crc_err=0
      echo "$mb,$r,$elapsed,1,$crc_err" >>"$CSV"
    else
      end=$(date +%s%3N)
      elapsed=$((end - start))
      echo "$mb,$r,$elapsed,0,0" >>"$CSV"
    fi
  done
done

python3 - "$CSV" "$OUT_DIR/large_file_summary.md" <<'PY'
import csv, sys
from collections import defaultdict
csv_path, out_md = sys.argv[1], sys.argv[2]
by_size = defaultdict(list)
with open(csv_path, newline='', encoding='utf-8') as f:
    for row in csv.DictReader(f):
        by_size[row['size_mb']].append(row)
with open(out_md, 'w', encoding='utf-8') as o:
    o.write('# 大文件分档传输\n\n')
    o.write('| 大小MB | 计划轮次 | 成功/总数 | 成功率% | 平均耗时s |\n')
    o.write('|--------|----------|-----------|---------|----------|\n')
    for mb in sorted(by_size.keys(), key=int):
        rows = by_size[mb]
        succ = sum(1 for r in rows if r['success']=='1')
        tot = len(rows)
        rate = 100.0*succ/tot if tot else 0
        times = [float(r['elapsed_ms'])/1000 for r in rows if r['success']=='1']
        avg_t = sum(times)/len(times) if times else 0
        o.write(f'| {mb} | {tot} | {succ}/{tot} | {rate:.1f} | {avg_t:.1f} |\n')
    o.write('\n开题参考：1GB 内成功率≥99.5%；CRC 错误见日志 grep。\n')
    o.write('轮次由环境变量 ROUNDS_10MB / ROUNDS_100MB / ROUNDS_1000MB（或 ROUNDS 默认）控制。\n')
PY

echo "[bench] -> $OUT_DIR/large_file_summary.md"
