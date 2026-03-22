#!/usr/bin/env bash
set -euo pipefail

# 文件作用说明：
# 文本消息RTT压测脚本（轻量版）。
# 指标输出：avg_ms、p95_ms、p99_ms、success_count。

PORT="${PORT:-8443}"
HOST="${HOST:-127.0.0.1}"
CERT_FILE="${CERT_FILE:-certs/server_cert.pem}"
KEY_FILE="${KEY_FILE:-certs/server_key.pem}"
CA_FILE="${CA_FILE:-certs/server_cert.pem}"
ROUNDS="${ROUNDS:-50}"
CONCURRENCY="${CONCURRENCY:-10}"
OUT_DIR="${OUT_DIR:-bench_output}"
mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/text_rtt.csv"

# CSV 表头：一行代表一个客户端一次请求。
echo "round,client,elapsed_ms,success" > "$CSV"

# 启动被测服务端实例。
./build/wss_server --port "$PORT" --cert "$CERT_FILE" --key "$KEY_FILE" > "$OUT_DIR/server_text.log" 2>&1 &
SERVER_PID=$!
cleanup() {
  # 退出时总是清理服务端进程，防止残留。
  kill "$SERVER_PID" >/dev/null 2>&1 || true
  wait "$SERVER_PID" >/dev/null 2>&1 || true
}
trap cleanup EXIT
sleep 1

run_one() {
  # 单次压测任务：发一条文本消息并记录耗时与成功标记。
  local r="$1"
  local c="$2"
  local start end elapsed
  start=$(date +%s%3N)
  if ./build/wss_client --host "$HOST" --port "$PORT" --ca "$CA_FILE" --text "bench-${r}-${c}" >/dev/null 2>&1; then
    end=$(date +%s%3N)
    elapsed=$((end - start))
    echo "${r},${c},${elapsed},1" >> "$CSV"
  else
    end=$(date +%s%3N)
    elapsed=$((end - start))
    echo "${r},${c},${elapsed},0" >> "$CSV"
  fi
}

export -f run_one
export HOST PORT CA_FILE CSV

# 外层轮次，内层并发客户端。
for ((r=1; r<=ROUNDS; r++)); do
  seq 1 "$CONCURRENCY" | xargs -I{} -P "$CONCURRENCY" bash -c 'run_one "$@"' _ "$r" "{}"
done

# 汇总 CSV 到 Markdown 报告，输出均值与分位点。
python3 - "$CSV" "$OUT_DIR/text_rtt_summary.md" <<'PY'
import csv, sys, statistics
csv_path, out_md = sys.argv[1], sys.argv[2]
vals = []
succ = 0
tot = 0
with open(csv_path, newline='', encoding='utf-8') as f:
    for row in csv.DictReader(f):
        tot += 1
        if row["success"] == "1":
            succ += 1
            vals.append(float(row["elapsed_ms"]))
vals.sort()
def pct(p):
    if not vals:
        return 0.0
    i = max(0, min(len(vals)-1, int(round((p/100.0)*(len(vals)-1)))))
    return vals[i]
avg = statistics.mean(vals) if vals else 0.0
with open(out_md, "w", encoding="utf-8") as o:
    o.write("# 文本RTT压测结果\n\n")
    o.write(f"- 总请求数: {tot}\n")
    o.write(f"- 成功数: {succ}\n")
    o.write(f"- 平均RTT(ms): {avg:.2f}\n")
    o.write(f"- P95(ms): {pct(95):.2f}\n")
    o.write(f"- P99(ms): {pct(99):.2f}\n")
PY

echo "文本RTT压测完成：$CSV"
echo "汇总报告：$OUT_DIR/text_rtt_summary.md"

