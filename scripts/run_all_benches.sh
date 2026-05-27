#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TS="$(date +%Y%m%d_%H%M%S)"
BASE="${OUT_BASE:-bench_output}/${TS}"
mkdir -p "$BASE"

export CERT_FILE="${CERT_FILE:-certs/server_cert.pem}"
export KEY_FILE="${KEY_FILE:-certs/server_key.pem}"
export CA_FILE="${CA_FILE:-certs/server_cert.pem}"

echo "[run_all] 输出目录: $BASE"
cmake -S . -B build >/dev/null
cmake --build build -j
./scripts/gen_cert.sh certs 2>/dev/null || true

run_step() {
  local name="$1"
  shift
  echo ""
  echo "======== $name ========"
  OUT_DIR="$BASE/$name" "$@" || echo "[run_all] WARN: $name 非零退出"
}

run_step smoke ./scripts/functional_smoke.sh

if [[ "${QUICK:-0}" == "1" ]]; then
  export SKIP_1GB=1
  export SKIP_VALGRIND=1
  export SKIP_HEAVY=1
  export HOLD_TARGETS="50 100"
  export HOLD_SECONDS=5
  export ROUNDS=5
  export SAMPLES=5
  export SIZES_MB="10"
  export BENCH_LARGE_ROUNDS_10=2
  export BENCH_LARGE_ROUNDS_100=1
fi

# 与 shell 中 export ROUNDS=50（会话复用）解耦，避免文本/吞吐压测被误放大。
TEXT_ROUNDS="${BENCH_TEXT_ROUNDS:-20}"
FILE_ROUNDS="${BENCH_FILE_ROUNDS:-5}"
SESSION_ROUNDS="${BENCH_SESSION_ROUNDS:-${ROUNDS:-50}}"

# 大文件分档轮次（互不影响；未设则 large_file 脚本内 ROUNDS 默认=3）
LARGE_ROUNDS_10="${BENCH_LARGE_ROUNDS_10:-${ROUNDS_10MB:-20}}"
LARGE_ROUNDS_100="${BENCH_LARGE_ROUNDS_100:-${ROUNDS_100MB:-3}}"
LARGE_ROUNDS_1000="${BENCH_LARGE_ROUNDS_1000:-${ROUNDS_1000MB:-1}}"

run_step text_rtt env ROUNDS="$TEXT_ROUNDS" CONCURRENCY="${CONCURRENCY:-5}" PORT=18450 ./scripts/bench_text_rtt.sh
run_step file_throughput env ROUNDS="$FILE_ROUNDS" CONCURRENCY=2 FILE_SIZE_MB=2 PORT=18451 ./scripts/bench_file_throughput.sh
run_step concurrent_hold env PORT=18452 ./scripts/bench_concurrent_hold.sh
run_step session_reuse env PORT=18453 ROUNDS="$SESSION_ROUNDS" ./scripts/bench_session_reuse.sh
run_step tls_compare env SAMPLES="${SAMPLES:-10}" ./scripts/bench_tls_handshake_compare.sh
run_step large_file env PORT=18454 \
  BENCH_LARGE_ROUNDS_10="$LARGE_ROUNDS_10" \
  BENCH_LARGE_ROUNDS_100="$LARGE_ROUNDS_100" \
  BENCH_LARGE_ROUNDS_1000="$LARGE_ROUNDS_1000" \
  SIZES_MB="${SIZES_MB:-10 100}" \
  BENCH_INCLUDE_1GB="${BENCH_INCLUDE_1GB:-0}" \
  SKIP_1GB="${SKIP_1GB:-0}" \
  ./scripts/bench_large_file.sh
run_step security env PORT=18455 ./scripts/security_check.sh

if [[ "${SKIP_VALGRIND:-0}" != "1" ]]; then
  run_step valgrind env PORT=18456 ./scripts/run_valgrind.sh
fi

INDEX="$BASE/INDEX.md"
{
  echo "# 压测归档索引"
  echo ""
  echo "- 时间戳: $TS"
  echo "- QUICK=${QUICK:-0} SKIP_HEAVY=${SKIP_HEAVY:-0} SKIP_1GB=${SKIP_1GB:-0}"
  echo "- 大文件轮次: 10MB=${LARGE_ROUNDS_10:-?} 100MB=${LARGE_ROUNDS_100:-?} 1000MB=${LARGE_ROUNDS_1000:-?}"
  echo ""
  for sub in smoke text_rtt file_throughput concurrent_hold session_reuse tls_compare large_file security valgrind; do
    if [[ -d "$BASE/$sub" ]]; then
      echo "## $sub"
      find "$BASE/$sub" -maxdepth 1 \( -name '*summary*.md' -o -name '*result*.md' -o -name 'smoke_result.txt' \) 2>/dev/null | sort | while read -r f; do
        echo "- [$(basename "$f")]($sub/$(basename "$f"))"
      done
      echo ""
    fi
  done
} >"$INDEX"

echo ""
echo "[run_all] 完成 -> $BASE"
echo "[run_all] 索引 -> $INDEX"
