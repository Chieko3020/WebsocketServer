#!/usr/bin/env bash
set -euo pipefail

# 文件作用说明：
# 文件吞吐与续传成功率压测脚本（轻量版）。
# 指标输出：总吞吐MB/s、成功率、平均单文件耗时。

PORT="${PORT:-8444}"
HOST="${HOST:-127.0.0.1}"
CERT_FILE="${CERT_FILE:-certs/server_cert.pem}"
KEY_FILE="${KEY_FILE:-certs/server_key.pem}"
CA_FILE="${CA_FILE:-certs/server_cert.pem}"
CONCURRENCY="${CONCURRENCY:-5}"
ROUNDS="${ROUNDS:-10}"
FILE_SIZE_MB="${FILE_SIZE_MB:-2}"
OUT_DIR="${OUT_DIR:-bench_output}"
mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/file_throughput.csv"
# CSV 表头：记录每个并发任务耗时与结果。
echo "round,client,elapsed_ms,success,file_mb" > "$CSV"

# 生成固定体积测试文件（若不存在）。
TMP_FILE="$OUT_DIR/bench_file_${FILE_SIZE_MB}mb.bin"
if [[ ! -f "$TMP_FILE" ]]; then
  dd if=/dev/urandom of="$TMP_FILE" bs=1M count="$FILE_SIZE_MB" status=none
fi

# 启动被测服务端实例（先清理残留进程）。
pkill -f "wss_server --port $PORT" 2>/dev/null || true
sleep 0.3
./build/wss_server --port "$PORT" --cert "$CERT_FILE" --key "$KEY_FILE" \
  --http-port 0 --no-log-file > "$OUT_DIR/server_file.log" 2>&1 &
SERVER_PID=$!
cleanup() {
  # 脚本退出时回收后台服务进程，避免端口占用。
  kill "$SERVER_PID" >/dev/null 2>&1 || true
  wait "$SERVER_PID" >/dev/null 2>&1 || true
  pkill -f "wss_server --port $PORT" 2>/dev/null || true
}
trap cleanup EXIT
sleep 1

csv_append() {
  { flock -x 9; echo "$1" >> "$CSV"; } 9>"$OUT_DIR/.file_throughput.csv.lock"
}

run_one() {
  # 单次任务：执行一次文件上传流程并记录耗时。
  local r="$1"
  local c="$2"
  local start end elapsed
  local task_file="$OUT_DIR/bench_file_${FILE_SIZE_MB}mb_r${r}_c${c}.bin"
  if [[ ! -f "$task_file" ]]; then
    cp "$TMP_FILE" "$task_file"
  fi
  start=$(date +%s%3N)
  if ./build/wss_client --host "$HOST" --port "$PORT" --ca "$CA_FILE" --text "bench-file" --file "$task_file" --chunk-size 32768 --client-ping-interval 5 >/dev/null 2>&1; then
    end=$(date +%s%3N)
    elapsed=$((end - start))
    csv_append "${r},${c},${elapsed},1,${FILE_SIZE_MB}"
  else
    end=$(date +%s%3N)
    elapsed=$((end - start))
    csv_append "${r},${c},${elapsed},0,${FILE_SIZE_MB}"
  fi
}

export -f run_one csv_append
export HOST PORT CA_FILE TMP_FILE CSV FILE_SIZE_MB OUT_DIR

# 外层轮次，内层并发上传任务。
for ((r=1; r<=ROUNDS; r++)); do
  seq 1 "$CONCURRENCY" | xargs -I{} -P "$CONCURRENCY" bash -c 'run_one "$@"' _ "$r" "{}"
done

# 汇总吞吐、成功率与平均耗时指标。
python3 - "$CSV" "$OUT_DIR/file_throughput_summary.md" <<'PY'
import csv, sys, statistics
csv_path, out_md = sys.argv[1], sys.argv[2]
rows = []
with open(csv_path, newline='', encoding='utf-8') as f:
    rows = list(csv.DictReader(f))
tot = len(rows)
succ_rows = [r for r in rows if r["success"] == "1"]
succ = len(succ_rows)
elapsed_sum_s = sum(float(r["elapsed_ms"]) for r in succ_rows) / 1000.0 if succ_rows else 0.0
mb_sum = sum(float(r["file_mb"]) for r in succ_rows)
throughput = (mb_sum / elapsed_sum_s) if elapsed_sum_s > 0 else 0.0
avg_elapsed = statistics.mean([float(r["elapsed_ms"]) for r in succ_rows]) if succ_rows else 0.0
rate = (succ/tot*100 if tot else 0)
succ_pass = "PASS" if rate >= 99.5 else "FAIL"
with open(out_md, "w", encoding="utf-8") as o:
    o.write("# 文件吞吐压测结果\n\n")
    o.write(f"- 总任务数: {tot}\n")
    o.write(f"- 成功数: {succ}\n")
    o.write(f"- **成功率**: {rate:.2f}%（开题≥99.5%: **{succ_pass}**）\n")
    o.write(f"- 总吞吐(MB/s): {throughput:.2f}\n")
    o.write(f"- 平均单文件耗时(ms): {avg_elapsed:.2f}\n")
    o.write(f"- 文件大小: {rows[0]['file_mb'] if rows else 'n/a'} MB\n")
PY

echo "文件吞吐压测完成：$CSV"
echo "汇总报告：$OUT_DIR/file_throughput_summary.md"

