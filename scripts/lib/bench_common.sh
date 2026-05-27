#!/usr/bin/env bash
# 压测脚本公共库：证书检查、服务端启停、指标解析。

bench_root_dir() {
  local script_path
  script_path="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  echo "$script_path"
}

bench_check_certs() {
  local cert="${CERT_FILE:-certs/server_cert.pem}"
  local key="${KEY_FILE:-certs/server_key.pem}"
  if [[ ! -f "$cert" || ! -f "$key" ]]; then
    echo "[bench] 证书缺失，运行 ./scripts/gen_cert.sh certs" >&2
    (cd "$(bench_root_dir)" && ./scripts/gen_cert.sh certs) || return 1
  fi
}

bench_start_server() {
  local log_file="$1"
  shift
  local root
  root="$(bench_root_dir)"
  cd "$root" || return 1
  bench_check_certs || return 1
  # 避免上次压测残留进程占用端口
  pkill -f './build/wss_server' 2>/dev/null || true
  sleep 1
  ./build/wss_server "$@" >"$log_file" 2>&1 &
  BENCH_SERVER_PID=$!
  sleep 1
  if ! kill -0 "$BENCH_SERVER_PID" 2>/dev/null; then
    echo "[bench] 服务端启动失败，日志: $log_file" >&2
    tail -n 30 "$log_file" >&2 || true
    return 1
  fi
}

bench_stop_server() {
  if [[ -n "${BENCH_SERVER_PID:-}" ]]; then
    kill "$BENCH_SERVER_PID" >/dev/null 2>&1 || true
    wait "$BENCH_SERVER_PID" >/dev/null 2>&1 || true
    unset BENCH_SERVER_PID
  fi
}

# 从服务端日志解析「指标」行中的握手统计（中文日志格式）。
bench_parse_server_metrics() {
  local log_file="$1"
  local hs_ok=0 hs_fail=0 hs_new=0 hs_reused=0
  if [[ ! -f "$log_file" ]]; then
    echo "0 0 0 0"
    return
  fi
  while IFS= read -r line; do
    [[ "$line" != *"指标"* ]] && continue
    if [[ "$line" =~ 握手成功[=:]([0-9]+) ]]; then hs_ok="${BASH_REMATCH[1]}"; fi
    if [[ "$line" =~ 握手失败[=:]([0-9]+) ]]; then hs_fail="${BASH_REMATCH[1]}"; fi
    if [[ "$line" =~ 新握手[=:]([0-9]+) ]]; then hs_new="${BASH_REMATCH[1]}"; fi
    if [[ "$line" =~ 复用握手[=:]([0-9]+) ]]; then hs_reused="${BASH_REMATCH[1]}"; fi
  done <"$log_file"
  echo "$hs_ok $hs_fail $hs_new $hs_reused"
}

bench_check_ulimit() {
  local target="${1:-1000}"
  local cur
  cur="$(ulimit -n)"
  if [[ "$cur" -lt 4096 && "$target" -ge 500 ]]; then
    echo "[bench] 警告: ulimit -n=$cur，建议 >=4096（目标并发 $target）" >&2
  fi
}
