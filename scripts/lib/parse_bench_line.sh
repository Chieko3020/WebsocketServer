#!/usr/bin/env bash
# 解析 wss_client 输出的 WSS_BENCH 行到环境变量。

parse_wss_bench_line() {
  local line="$1"
  BENCH_OK=""
  BENCH_HANDSHAKE_MS=""
  BENCH_SESSION_REUSED=""
  BENCH_FAIL_REASON=""
  if [[ "$line" =~ WSS_BENCH[[:space:]]+ok[[:space:]]+handshake_ms=([0-9]+)[[:space:]]+session_reused=([0-9]+) ]]; then
    BENCH_OK=1
    BENCH_HANDSHAKE_MS="${BASH_REMATCH[1]}"
    BENCH_SESSION_REUSED="${BASH_REMATCH[2]}"
    return 0
  fi
  if [[ "$line" =~ WSS_BENCH[[:space:]]+fail[[:space:]]+reason=(.*) ]]; then
    BENCH_OK=0
    BENCH_FAIL_REASON="${BASH_REMATCH[1]}"
    return 1
  fi
  return 2
}
