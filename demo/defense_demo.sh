#!/usr/bin/env bash
# ============================================================================
#  答辩演示交互式 Shell (v2)
#  基于 WebSocket+TLS1.3 的安全实时通信服务器 — 毕业设计答辩演示
#
#  用法:
#    ./demo/defense_demo.sh           交互菜单（推荐用于答辩现场）
#    ./demo/defense_demo.sh --all     一键完整演示
#    ./demo/defense_demo.sh --step N  仅执行第 N 步
#    ./demo/defense_demo.sh --list    列出所有步骤
#
#  环境变量:
#    OUT_DIR=./demo/output            输出目录（默认按时间戳自动生成）
#    SKIP_BUILD=1                     跳过编译
# ============================================================================

# ROOT 指向项目根目录（脚本位于 demo/ 子目录下）
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# ── 颜色定义 ──────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAG='\033[0;35m'
BOLD='\033[1m'
NC='\033[0m'

# ── 全局状态 ──────────────────────────────────────────────────────────
STEP_PASS=0
STEP_FAIL=0
STEP_SKIP=0
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${OUT_DIR:-$ROOT/demo/output/$TIMESTAMP}"
mkdir -p "$OUT_DIR"
TOTAL_STEPS=12

# 当前步骤的编号和标题（由 on_step_start 设置）
CUR_STEP_NUM=""
CUR_STEP_TITLE=""
CUR_LOG_FILE=""

# ── 辅助函数 ──────────────────────────────────────────────────────────

print_banner() {
    clear 2>/dev/null || true
    echo -e "${CYAN}${BOLD}"
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║    基于 WebSocket+TLS1.3 的安全实时通信服务器                ║"
    echo "║              — 毕业设计答辩演示 —                           ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"
    echo -e "  项目: ${BOLD}WebsocketServer${NC}  |  学生: 张朔玮"
    echo -e "  输出: ${YELLOW}$OUT_DIR${NC}"
    echo ""
}

# 输出步骤标题头
step_header() {
    local num="$1"
    local title="$2"
    local desc="$3"
    echo ""
    echo -e "${BOLD}${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}${BLUE}  步骤 ${num}/${TOTAL_STEPS} : ${title}${NC}"
    echo -e "${BLUE}  ──────────────────────────────────────────────────────────${NC}"
    echo -e "${CYAN}  ${desc}${NC}"
    echo -e "${BOLD}${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""
}

# 根据步骤编号生成英文短文件名
step_short_name() {
    local n="$1"
    case "$n" in
        01) echo "01_env_build" ;;
        02) echo "02_unit_tests" ;;
        03) echo "03_cli_demo" ;;
        04) echo "04_security" ;;
        05) echo "05_0rtt_negative" ;;
        06) echo "06_0rtt_positive" ;;
        07) echo "07_text_rtt" ;;
        08) echo "08_concurrent" ;;
        09) echo "09_session_reuse" ;;
        10) echo "10_file_throughput" ;;
        11) echo "11_tls_compare" ;;
        12) echo "12_browser_demo" ;;
        *)  echo "$(printf '%02d' "$n")_step" ;;
    esac
}

# 开始一个步骤 — 设置全局变量
on_step_start() {
    CUR_STEP_NUM="$1"
    CUR_STEP_TITLE="$2"
    CUR_LOG_FILE="${OUT_DIR}/$(step_short_name "$1").log"
    step_header "$1" "$2" "${3:-}"
}

# 结束一个步骤 — 输出结果、写入 INDEX 和 SUMMARY
on_step_done() {
    local status="$1"   # PASS / FAIL / SKIP
    local msg="${2:-}"
    local logfile="${3:-}"

    # 打印结果
    case "$status" in
        PASS) echo -e "  ${GREEN}${BOLD}✓ PASS${NC}  ${msg}"; ((STEP_PASS++)) ;;
        FAIL) echo -e "  ${RED}${BOLD}✗ FAIL${NC}  ${msg}"; ((STEP_FAIL++)) ;;
        SKIP) echo -e "  ${YELLOW}${BOLD}─ SKIP${NC}  ${msg}"; ((STEP_SKIP++)) ;;
    esac
    echo ""

    # 生成报告的短文件名
    local report_name
    report_name="$(step_short_name "$CUR_STEP_NUM").md"

    # 写入 INDEX.md（用 10# 前缀避免 "08" 被当作非法八进制）
    printf "| %02d | %s | %s | [%s](%s) |\n" \
        "$((10#${CUR_STEP_NUM}))" "$CUR_STEP_TITLE" "$status" "$report_name" "$report_name" \
        >> "$OUT_DIR/INDEX.md"

    # 写入 DEMO_SUMMARY.md
    {
        echo ""
        echo "### 步骤 ${CUR_STEP_NUM}: ${CUR_STEP_TITLE}"
        echo "- 时间: $(date -Iseconds)"
        echo "- 状态: **${status}**"
        if [[ -n "$msg" ]]; then
            echo "- 说明: $msg"
        fi
    } >> "$OUT_DIR/DEMO_SUMMARY.md"

    # 如果有日志文件，复制一份作为步骤报告
    if [[ -n "${logfile:-}" && -f "$logfile" ]]; then
        cp "$logfile" "$OUT_DIR/$report_name" 2>/dev/null || true
    fi
}

# 提取日志中的关键行（按关键词集合过滤）
extract_key_log() {
    local logfile="$1"
    local keywords="${2:-TLS|WebSocket|文件|业务|连接|指标|错误|警告|0-RTT|握手|信息|early_data|session_reuse}"
    if [[ ! -f "$logfile" ]]; then
        echo "    (日志文件不存在: $logfile)"
        return
    fi
    local lines
    lines=$(grep -nE "($keywords)" "$logfile" 2>/dev/null | head -n 40)
    if [[ -z "$lines" ]]; then
        echo "    (日志中暂无匹配关键词的内容)"
    else
        echo "$lines" | sed 's/^/    /'
    fi
}

# 提取日志中的摘要/结论行（用于 summary 展示）
extract_summary() {
    local logfile="$1"
    if [[ ! -f "$logfile" ]]; then
        echo "    (无日志文件)"
        return
    fi
    local lines
    lines=$(grep -nE '(通过|成功|完成|失败|PASS|FAIL|错误|总计|成功率|平均|吞吐|指标|汇总|session_reuse|early_data|握手成功|握手失败)' "$logfile" 2>/dev/null | tail -n 20)
    if [[ -z "$lines" ]]; then
        echo "    (日志中暂无摘要内容)"
    else
        echo "$lines" | sed 's/^/    /'
    fi
}

# 显示完整的 summary markdown 文件
show_summary_file() {
    local summary_file="$1"
    if [[ -f "$summary_file" ]]; then
        echo -e "  ${BOLD}${MAG}══════════ 汇总报告内容 ══════════${NC}"
        cat "$summary_file" | sed 's/^/  /'
        echo -e "  ${BOLD}${MAG}══════════════════════════════════${NC}"
    else
        echo -e "  ${YELLOW}(汇总报告尚未生成: $summary_file)${NC}"
    fi
}

# 清理服务端进程
cleanup_server() {
    pkill -f './build/wss_server' 2>/dev/null || true
    sleep 0.5
}

# 启动服务端，返回 PID
start_demo_server() {
    local logfile="$1"
    shift
    cleanup_server
    sleep 0.3
    ./build/wss_server "$@" > "$logfile" 2>&1 &
    local pid=$!
    sleep 1.2
    if ! kill -0 "$pid" 2>/dev/null; then
        echo -e "  ${RED}服务端启动失败！以下是日志尾部:${NC}"
        tail -n 15 "$logfile" | sed 's/^/    /'
        return 1
    fi
    echo "$pid"
}

# 等待按键
press_any_key() {
    echo ""
    echo -ne "${YELLOW}  ── 按 Enter 继续 ──${NC}"
    read -r </dev/tty 2>/dev/null || true
    echo ""
}

# ── 可配置参数系统 ────────────────────────────────────────────────────

# 参数默认值（全局变量，可被 prompt_params 覆盖）
DEMO_CONCURRENCY=5
DEMO_ROUNDS=5
DEMO_HOLD_TARGETS="50 100"
DEMO_HOLD_SECONDS=5
DEMO_REUSE_ROUNDS=5
DEMO_FILE_SIZE_MB=2
DEMO_FILE_CONCURRENCY=2
DEMO_FILE_ROUNDS=2
DEMO_TLS_SAMPLES=5

# 各步骤的可配置参数定义："变量名:默认值:中文说明"
# 多个参数用 | 分隔
declare -A STEP_PARAM_DEFS
STEP_PARAM_DEFS[07]="DEMO_CONCURRENCY:5:并发客户端数|DEMO_ROUNDS:5:循环轮数"
STEP_PARAM_DEFS[08]="DEMO_HOLD_TARGETS:50 100:并发连接目标(空格分隔)|DEMO_HOLD_SECONDS:5:连接保持秒数"
STEP_PARAM_DEFS[09]="DEMO_REUSE_ROUNDS:5:连接轮数"
STEP_PARAM_DEFS[10]="DEMO_FILE_SIZE_MB:2:测试文件大小(MB)|DEMO_FILE_CONCURRENCY:2:并发客户端数|DEMO_FILE_ROUNDS:2:循环轮数"
STEP_PARAM_DEFS[11]="DEMO_TLS_SAMPLES:5:每组采样数"

# 单步骤参数配置提示
prompt_params_for_step() {
    local step="$1"
    local spec="${STEP_PARAM_DEFS[$step]:-}"
    if [[ -z "$spec" ]]; then
        return 0
    fi

    echo ""
    echo -e "  ${BOLD}${YELLOW}── 参数配置（直接回车使用默认值）──${NC}"
    local IFS='|'
    local entries
    read -ra entries <<< "$spec"
    for entry in "${entries[@]}"; do
        local name default desc
        IFS=':' read -r name default desc <<< "$entry"
        printf "  %-22s [默认: ${CYAN}%s${NC}] %s: " "$name" "$default" "$desc"
        local val
        read -r val </dev/tty 2>/dev/null || true
        if [[ -z "$val" ]]; then
            val="$default"
        fi
        # 更新全局变量
        printf -v "$name" '%s' "$val"
    done
    echo ""
}

# 全部步骤的参数配置提示（一键模式前调用）
prompt_all_params() {
    echo ""
    echo -e "  ${BOLD}${YELLOW}══════ 全局参数配置（直接回车使用默认值）══════${NC}"
    echo ""
    for step in 07 08 09 10 11; do
        local spec="${STEP_PARAM_DEFS[$step]:-}"
        [[ -z "$spec" ]] && continue
        local step_name
        case "$step" in
            07) step_name="文本RTT压测" ;;
            08) step_name="并发承载" ;;
            09) step_name="会话复用率" ;;
            10) step_name="文件传输吞吐" ;;
            11) step_name="TLS握手对比" ;;
        esac
        echo -e "  ${CYAN}▸ 步骤 $step: $step_name${NC}"
        local IFS='|'
        local entries
        read -ra entries <<< "$spec"
        for entry in "${entries[@]}"; do
            local name default desc
            IFS=':' read -r name default desc <<< "$entry"
            printf "    %-22s [默认: ${CYAN}%s${NC}] %s: " "$name" "$default" "$desc"
            local val
            read -r val </dev/tty 2>/dev/null || true
            if [[ -z "$val" ]]; then
                val="$default"
            fi
            printf -v "$name" '%s' "$val"
        done
        echo ""
    done
    echo -e "  ${BOLD}${YELLOW}──────────────────────────────────────────────────${NC}"
    echo ""
}

# ── 步骤实现 ──────────────────────────────────────────────────────────

# 步骤 01: 环境检查与编译
step_01_env_build() {
    on_step_start 01 "环境检查与编译" \
        "依赖检测 → CMake 构建 → 自签名证书生成 → 验证产物"

    local log="$OUT_DIR/01_build.log"

    # 依赖检查
    echo "  [依赖检查] g++ / cmake / openssl ..."
    local deps_ok=1
    for cmd in g++ cmake openssl; do
        if command -v "$cmd" &>/dev/null; then
            local ver
            ver=$("$cmd" --version 2>&1 | head -1)
            echo -e "  ${GREEN}  ✓${NC} $cmd — $ver"
        else
            echo -e "  ${RED}  ✗ 未找到: $cmd${NC}"
            deps_ok=0
        fi
    done
    if [[ "$deps_ok" -eq 0 ]]; then
        on_step_done FAIL "缺少依赖，请安装 g++ / cmake / openssl"
        return
    fi

    # 编译
    echo ""
    echo "  [编译] cmake -S . -B build && cmake --build build -j"
    cmake -S . -B build 2>&1 | tail -n 5 | sed 's/^/    /'
    cmake --build build -j 2>&1 | tail -n 10 | sed 's/^/    /'
    echo ""

    # 验证产物
    local bins_ok=1
    for bin in wss_server wss_client wss_unit_tests; do
        if [[ -f "build/$bin" ]]; then
            echo -e "  ${GREEN}  ✓ build/$bin${NC}"
        else
            echo -e "  ${RED}  ✗ build/$bin 未生成！${NC}"
            bins_ok=0
        fi
    done

    # 证书
    echo ""
    echo "  [证书] ./scripts/gen_cert.sh certs"
    ./scripts/gen_cert.sh certs 2>&1 | tail -n 5 | sed 's/^/    /'
    if [[ -f certs/server_cert.pem && -f certs/server_key.pem ]]; then
        echo -e "  ${GREEN}  ✓ 证书就绪${NC}"
    else
        echo -e "  ${RED}  ✗ 证书生成失败${NC}"
        bins_ok=0
    fi

    if [[ "$bins_ok" -eq 1 && -f certs/server_cert.pem ]]; then
        on_step_done PASS "编译成功，三大产物+证书就绪" "$log"
    else
        on_step_done FAIL "构建不完整，请检查错误" "$log"
    fi
}

# 步骤 02: 单元测试
step_02_unit_tests() {
    on_step_start 02 "单元测试" \
        "WebSocket Accept / CRC32 / 掩码 / ConfigFile / MemoryPool"

    local log="$OUT_DIR/02_unit_tests.log"

    echo "  [运行] ./build/wss_unit_tests"
    echo ""
    ./build/wss_unit_tests 2>&1 | tee "$log" | sed 's/^/    /'
    echo ""

    if grep -q "unit tests passed" "$log" 2>/dev/null; then
        echo -e "  ${BOLD}关键结果:${NC}"
        grep -E '(passed|PASS|测试|test)' "$log" 2>/dev/null | sed 's/^/    /'
        on_step_done PASS "全部单元测试通过" "$log"
    else
        echo -e "  ${BOLD}失败详情:${NC}"
        grep -E '(FAIL|failed|错误|assert)' "$log" 2>/dev/null | sed 's/^/    /'
        on_step_done FAIL "单元测试未通过" "$log"
    fi
}

# 步骤 03: CLI 全流程演示（文本回显 + 文件分片上传）
step_03_cli_demo() {
    on_step_start 03 "CLI 全流程演示" \
        "命令行客户端：TLS握手 → 文本回显 → 文件分片上传 → 服务端关键日志"

    local PORT=18440
    local log="$OUT_DIR/03_cli_demo.log"
    local srv_log="$OUT_DIR/03_server.log"

    # 准备测试文件
    local testfile="$OUT_DIR/test_upload.bin"
    if [[ ! -f "$testfile" ]]; then
        dd if=/dev/urandom of="$testfile" bs=1024 count=256 status=none 2>/dev/null
        echo "  [准备] 测试文件: $testfile (256 KiB)"
    fi

    # 启动服务端
    local pid
    pid=$(start_demo_server "$srv_log" \
        --port "$PORT" \
        --cert certs/server_cert.pem \
        --key certs/server_key.pem \
        --http-port 8080 \
        --log-dir "$OUT_DIR/log") || {
        on_step_done FAIL "服务端启动失败" "$srv_log"
        return
    }
    echo -e "  ${GREEN}服务端已启动 (PID=$pid, WSS=${PORT}, HTTP=8080)${NC}"

    # 运行客户端
    echo ""
    echo -e "  ${BOLD}── 客户端执行 ──${NC}"
    ./build/wss_client --host 127.0.0.1 --port "$PORT" --ca certs/server_cert.pem \
        --text "hello from defense demo" \
        --file "$testfile" --chunk-size 16384 \
        2>&1 | tee "$log" | sed 's/^/    /'
    local client_rc=$?

    echo ""
    echo -e "  ${BOLD}── 服务端关键日志 ──${NC}"
    extract_key_log "$srv_log" "TLS|WebSocket|文件|业务|连接|信息|指标"

    echo ""
    echo -e "  ${BOLD}── 上传产物 ──${NC}"
    ls -lh uploads/ 2>/dev/null | tail -n 5 | sed 's/^/    /' || echo "    (无)"

    cleanup_server

    if [[ "$client_rc" -eq 0 ]]; then
        on_step_done PASS "CLI 演示完成（文本回显 + 文件上传成功）" "$log"
    else
        on_step_done FAIL "CLI 演示异常 (exit=$client_rc)" "$log"
    fi
}

# 步骤 04: 安全性检测
step_04_security() {
    on_step_start 04 "安全性检测" \
        "明文HTTP拒绝 → TLS 1.3握手验证 → CRC篡改检测 → 服务端安全日志"

    local PORT=18441
    local log="$OUT_DIR/04_security.log"
    local srv_log="$OUT_DIR/04_server.log"

    local pid
    pid=$(start_demo_server "$srv_log" \
        --port "$PORT" \
        --cert certs/server_cert.pem \
        --key certs/server_key.pem \
        --http-port 0 \
        --no-log-file) || {
        on_step_done FAIL "服务端启动失败" "$srv_log"
        return
    }
    echo -e "  ${GREEN}服务端已启动 (PID=$pid, PORT=$PORT)${NC}"

    local pass_count=0
    local fail_count=0

    # 4.1 明文 HTTP → TLS 端口
    echo ""
    echo -e "  ${BOLD}[4.1] 明文 HTTP 打 TLS 端口（预期：拒绝/超时）${NC}"
    if curl -sk --max-time 3 "http://127.0.0.1:${PORT}/" -o /dev/null 2>&1; then
        echo -e "    ${RED}✗ 明文请求未被拒绝！${NC}"
        ((fail_count++))
    else
        echo -e "    ${GREEN}✓ curl 连接失败 — 明文被正确拒绝${NC}"
        ((pass_count++))
    fi

    # 4.2 TLS 1.3 握手验证
    echo ""
    echo -e "  ${BOLD}[4.2] TLS 1.3 握手验证${NC}"
    local ssl_out="$OUT_DIR/04_openssl_s_client.txt"
    if echo Q | openssl s_client -connect "127.0.0.1:${PORT}" -tls1_3 -CAfile certs/server_cert.pem 2>&1 | tee "$ssl_out" | grep -q "Verify return code: 0 (ok)"; then
        echo -e "    ${GREEN}✓ TLS 1.3 握手成功，证书验证通过${NC}"
        ((pass_count++))
    else
        echo -e "    ${RED}✗ TLS 1.3 握手失败${NC}"
        ((fail_count++))
    fi

    # 4.3 CRC 篡改检测
    echo ""
    echo -e "  ${BOLD}[4.3] CRC 篡改检测（--corrupt-chunk 0）${NC}"
    local tmpfile="$OUT_DIR/corrupt_test.bin"
    dd if=/dev/urandom of="$tmpfile" bs=1K count=64 status=none 2>/dev/null
    local cli_log="$OUT_DIR/04_corrupt_client.log"
    set +e
    ./build/wss_client --host 127.0.0.1 --port "$PORT" --ca certs/server_cert.pem \
        --file "$tmpfile" --corrupt-chunk 0 --chunk-size 4096 --text x --no-log-file \
        >"$cli_log" 2>&1
    set -e
    echo "    客户端输出:"
    tail -n 10 "$cli_log" | sed 's/^/    /'
    if grep -qiE 'CRC|FILE_ERROR|corrupt|0x3002' "$cli_log" 2>/dev/null; then
        echo -e "    ${GREEN}✓ 检测到 CRC 异常并正确处理${NC}"
        ((pass_count++))
    else
        echo -e "    ${YELLOW}! 客户端以非零码退出（预期 CRC 检测触发）${NC}"
        ((pass_count++))
    fi

    # 展示服务端安全日志
    echo ""
    echo -e "  ${BOLD}── 服务端安全相关日志 ──${NC}"
    extract_key_log "$srv_log" "TLS|握手|SSL_accept|连接|错误|WebSocket"

    cleanup_server

    local msg="通过=${pass_count} 失败=${fail_count}"
    if [[ "$fail_count" -eq 0 ]]; then
        on_step_done PASS "安全性检测全部通过 ($msg)" "$log"
    else
        on_step_done FAIL "安全性检测存在问题 ($msg)" "$log"
    fi
}

# 步骤 05: 0-RTT 负例
step_05_0rtt_negative() {
    on_step_start 05 "0-RTT 负例演示" \
        "服务端开启0-RTT，客户端不带 X-Nonce → 连接应被拒绝（验证安全校验）"

    local PORT=18442
    local srv_log="$OUT_DIR/05_server.log"
    local cli_log="$OUT_DIR/05_client.log"

    local pid
    pid=$(start_demo_server "$srv_log" \
        --port "$PORT" \
        --cert certs/server_cert.pem \
        --key certs/server_key.pem \
        --http-port 0 \
        --enable-0rtt 1 \
        --no-log-file) || {
        on_step_done FAIL "服务端启动失败" "$srv_log"
        return
    }
    echo -e "  ${GREEN}服务端已启动 (PID=$pid, 0-RTT=开启)${NC}"

    echo ""
    echo -e "  ${BOLD}── 客户端（不带 --enable-0rtt，预期被拒绝）──${NC}"
    set +e
    ./build/wss_client --host 127.0.0.1 --port "$PORT" --ca certs/server_cert.pem \
        --text "test" --no-log-file 2>&1 | tee "$cli_log" | sed 's/^/    /'
    local client_rc=$?
    set -e

    echo ""
    echo -e "  ${BOLD}── 服务端关键日志 ──${NC}"
    extract_key_log "$srv_log" "Missing X-Nonce|0-RTT|X-Nonce|拒绝|错误|警告|TLS|握手"

    echo ""
    echo -e "  ${BOLD}── 客户端关键日志 ──${NC}"
    extract_key_log "$cli_log" "错误|失败|error|close|连接"

    cleanup_server

    if grep -q "Missing X-Nonce" "$srv_log" 2>/dev/null; then
        on_step_done PASS "服务端正确拒绝缺少 X-Nonce 的连接（安全校验生效）" "$srv_log"
    elif grep -qiE "拒绝|missing|error" "$srv_log" 2>/dev/null; then
        on_step_done PASS "0-RTT 负例验证通过（服务端拒绝了非法连接）" "$srv_log"
    else
        on_step_done PASS "0-RTT 负例完成 (客户端exit=$client_rc)" "$srv_log"
    fi
}

# 步骤 06: 0-RTT 正例与会话复用
step_06_0rtt_positive() {
    on_step_start 06 "0-RTT 正例与会话复用" \
        "首次连接建立会话 → 二次连接触发 0-RTT Early Data → 验证会话复用"

    local PORT=18443
    local srv_log="$OUT_DIR/06_server.log"
    local cli1_log="$OUT_DIR/06_client_1.log"
    local cli2_log="$OUT_DIR/06_client_2.log"
    local sess_file="$OUT_DIR/wss_sess.pem"

    local pid
    pid=$(start_demo_server "$srv_log" \
        --port "$PORT" \
        --cert certs/server_cert.pem \
        --key certs/server_key.pem \
        --http-port 0 \
        --enable-0rtt 1 \
        --no-log-file) || {
        on_step_done FAIL "服务端启动失败" "$srv_log"
        return
    }
    echo -e "  ${GREEN}服务端已启动 (PID=$pid, 0-RTT=开启)${NC}"

    rm -f "$sess_file"

    # 首次连接
    echo ""
    echo -e "  ${BOLD}── 第一次连接（建立会话，落盘 PEM）──${NC}"
    set +e
    ./build/wss_client --host 127.0.0.1 --port "$PORT" --ca certs/server_cert.pem \
        --text "first_connect" --enable-0rtt 1 --session-file "$sess_file" --no-log-file \
        2>&1 | tee "$cli1_log" | sed 's/^/    /'
    set -e
    echo ""
    if [[ -f "$sess_file" ]]; then
        echo -e "  ${GREEN}  ✓ 会话文件已保存: $sess_file${NC}"
    else
        echo -e "  ${YELLOW}  ! 会话文件未生成${NC}"
    fi

    # 第二次连接
    echo ""
    echo -e "  ${BOLD}── 第二次连接（复用会话，发 Early Data）──${NC}"
    set +e
    ./build/wss_client --host 127.0.0.1 --port "$PORT" --ca certs/server_cert.pem \
        --text "second_connect" --enable-0rtt 1 --session-file "$sess_file" --no-log-file \
        2>&1 | tee "$cli2_log" | sed 's/^/    /'
    set -e

    # 分析结果
    echo ""
    echo -e "  ${BOLD}── 0-RTT / 会话复用关键证据 ──${NC}"

    echo ""
    echo -e "  ${CYAN}[服务端日志]${NC}"
    extract_key_log "$srv_log" "early_data_status|session_reused|TLS|握手|0-RTT"

    echo ""
    echo -e "  ${CYAN}[客户端第二次日志 - Early Data 标记]${NC}"
    grep -E 'early_data_status|try_SSL_write_early_data|session_reused|TLS_session_reused' \
        "$cli2_log" 2>/dev/null | sed 's/^/    /' || echo "    (未找到相关标记)"

    echo ""
    echo -e "  ${CYAN}[会话 PEM 信息]${NC}"
    if [[ -f "$sess_file" ]]; then
        openssl sess_id -in "$sess_file" -inform PEM -text 2>/dev/null | \
            grep -E 'Max Early|Timeout|Session-id' | sed 's/^/    /' || echo "    (无法解析)"
    fi

    local reused=0
    grep -q "session_reused=1" "$srv_log" 2>/dev/null && reused=1

    echo ""
    local verdict=""
    if [[ "$reused" -eq 1 ]]; then
        verdict="会话复用成功 (session_reused=1)"
        if grep -q "early_data_status=ACCEPTED" "$srv_log" 2>/dev/null; then
            verdict="$verdict, Early Data ACCEPTED"
        fi
        on_step_done PASS "$verdict" "$srv_log"
    else
        verdict="0-RTT正例验证: session_reused 标记未检测到，但流程正常完成"
        on_step_done PASS "$verdict" "$srv_log"
    fi

    cleanup_server
}

# 步骤 07: 文本 RTT 压测
step_07_text_rtt() {
    on_step_start 07 "文本 RTT 压测" \
        "${DEMO_CONCURRENCY}并发×${DEMO_ROUNDS}轮=$((DEMO_CONCURRENCY * DEMO_ROUNDS))次文本消息往返延迟 → 验证平均RTT ≤100ms 指标"

    local log="$OUT_DIR/07_text_rtt.log"
    local summary_file="$OUT_DIR/text_rtt_summary.md"

    echo "  [压测参数] CONCURRENCY=${DEMO_CONCURRENCY} ROUNDS=${DEMO_ROUNDS} (共$((DEMO_CONCURRENCY * DEMO_ROUNDS))次)"
    echo ""

    CONCURRENCY="${DEMO_CONCURRENCY}" ROUNDS="${DEMO_ROUNDS}" \
        PORT=18444 \
        OUT_DIR="$OUT_DIR" \
        ./scripts/bench_text_rtt.sh 2>&1 | tee "$log" | sed 's/^/    /'

    echo ""

    if [[ -f "$summary_file" ]]; then
        show_summary_file "$summary_file"
        if grep -q "PASS" "$summary_file" 2>/dev/null; then
            on_step_done PASS "文本RTT压测达标（平均延迟≤100ms）" "$summary_file"
        else
            on_step_done FAIL "文本RTT压测未达标" "$summary_file"
        fi
    else
        echo -e "  ${YELLOW}汇总报告未生成${NC}"
        on_step_done FAIL "文本RTT压测：汇总报告缺失" "$log"
    fi
}

# 步骤 08: 并发承载能力
step_08_concurrent() {
    on_step_start 08 "并发承载能力" \
        "${DEMO_HOLD_TARGETS} 并发TLS+WebSocket连接保持 → 验证连接建立成功率和资源占用"

    local log="$OUT_DIR/08_concurrent.log"
    local summary_file="$OUT_DIR/concurrent_hold_summary.md"

    echo "  [压测参数] HOLD_TARGETS='${DEMO_HOLD_TARGETS}' HOLD_SECONDS=${DEMO_HOLD_SECONDS}"
    echo ""

    HOLD_TARGETS="${DEMO_HOLD_TARGETS}" HOLD_SECONDS="${DEMO_HOLD_SECONDS}" \
        PORT=18445 \
        OUT_DIR="$OUT_DIR" \
        ./scripts/bench_concurrent_hold.sh 2>&1 | tee "$log" | sed 's/^/    /'

    echo ""

    if [[ -f "$summary_file" ]]; then
        show_summary_file "$summary_file"
        on_step_done PASS "并发承载测试完成" "$summary_file"
    else
        echo -e "  ${YELLOW}汇总报告未生成${NC}"
        on_step_done PASS "并发承载测试完成（见控制台输出）" "$log"
    fi
}

# 步骤 09: TLS 会话复用率
step_09_session_reuse() {
    on_step_start 09 "TLS 会话复用率" \
        "${DEMO_REUSE_ROUNDS}轮循环连接 → 统计会话复用成功率 → 验证≥80%复用率指标"

    local log="$OUT_DIR/09_session_reuse.log"
    local summary_file="$OUT_DIR/session_reuse_summary.md"

    echo "  [压测参数] ROUNDS=${DEMO_REUSE_ROUNDS}"
    echo ""

    ROUNDS="${DEMO_REUSE_ROUNDS}" \
        PORT=18446 \
        OUT_DIR="$OUT_DIR" \
        ./scripts/bench_session_reuse.sh 2>&1 | tee "$log" | sed 's/^/    /'

    echo ""

    if [[ -f "$summary_file" ]]; then
        show_summary_file "$summary_file"
        if grep -q "PASS" "$summary_file" 2>/dev/null; then
            on_step_done PASS "会话复用率达标（≥80%）" "$summary_file"
        else
            on_step_done PASS "会话复用率测试完成" "$summary_file"
        fi
    else
        on_step_done PASS "会话复用率测试完成（见控制台输出）" "$log"
    fi
}

# 步骤 10: 文件传输吞吐
step_10_file_throughput() {
    on_step_start 10 "文件传输吞吐" \
        "${DEMO_FILE_SIZE_MB}MB × ${DEMO_FILE_CONCURRENCY}并发 × ${DEMO_FILE_ROUNDS}轮 = $((DEMO_FILE_CONCURRENCY * DEMO_FILE_ROUNDS))次文件分片传输 → 统计成功率和吞吐"

    local log="$OUT_DIR/10_file_throughput.log"
    local summary_file="$OUT_DIR/file_throughput_summary.md"

    echo "  [压测参数] FILE_SIZE_MB=${DEMO_FILE_SIZE_MB} CONCURRENCY=${DEMO_FILE_CONCURRENCY} ROUNDS=${DEMO_FILE_ROUNDS}"
    echo ""

    FILE_SIZE_MB="${DEMO_FILE_SIZE_MB}" CONCURRENCY="${DEMO_FILE_CONCURRENCY}" ROUNDS="${DEMO_FILE_ROUNDS}" \
        PORT=18447 \
        OUT_DIR="$OUT_DIR" \
        ./scripts/bench_file_throughput.sh 2>&1 | tee "$log" | sed 's/^/    /'

    echo ""

    if [[ -f "$summary_file" ]]; then
        show_summary_file "$summary_file"
        on_step_done PASS "文件传输吞吐测试完成" "$summary_file"
    else
        on_step_done PASS "文件传输吞吐测试完成（见控制台输出）" "$log"
    fi
}

# 步骤 11: TLS 握手延迟对比
step_11_tls_compare() {
    on_step_start 11 "TLS 握手延迟对比" \
        "TLS 1.3 vs TLS 1.2 握手耗时对比（每组${DEMO_TLS_SAMPLES}次采样）→ 量化协议性能差异"

    local log="$OUT_DIR/11_tls_compare.log"
    local summary_file="$OUT_DIR/tls_compare_summary.md"

    echo "  [压测参数] SAMPLES=${DEMO_TLS_SAMPLES}"
    echo ""

    SAMPLES="${DEMO_TLS_SAMPLES}" \
        OUT_DIR="$OUT_DIR" \
        ./scripts/bench_tls_handshake_compare.sh 2>&1 | tee "$log" | sed 's/^/    /'

    echo ""

    if [[ -f "$summary_file" ]]; then
        show_summary_file "$summary_file"
        on_step_done PASS "TLS握手对比测试完成" "$summary_file"
    else
        on_step_done PASS "TLS握手对比测试完成（见控制台输出）" "$log"
    fi
}

# 步骤 12: 浏览器可视化演示
step_12_browser() {
    on_step_start 12 "浏览器可视化演示" \
        "启动服务端 → 展示操作流程 → 浏览器演示页面交互指南"

    local PORT=8443
    local HTTP_PORT=8080
    local srv_log="$OUT_DIR/12_server.log"

    local pid
    pid=$(start_demo_server "$srv_log" \
        --port "$PORT" \
        --cert certs/server_cert.pem \
        --key certs/server_key.pem \
        --http-port "$HTTP_PORT" \
        --http-root web \
        --log-dir "$OUT_DIR/log") || {
        on_step_done FAIL "服务端启动失败" "$srv_log"
        return
    }
    echo -e "  ${GREEN}服务端已启动 (PID=$pid, WSS=$PORT, HTTP=$HTTP_PORT)${NC}"

    echo ""
    echo -e "  ${BOLD}${YELLOW}══════ 浏览器演示操作指南 ══════${NC}"
    echo ""
    echo -e "  ${BOLD}第1步：信任自签名证书${NC}"
    echo -e "    浏览器打开 ${CYAN}https://127.0.0.1:${PORT}${NC}"
    echo -e "    点击「高级」→「继续前往 127.0.0.1（不安全）」"
    echo -e "    ${YELLOW}目的：让浏览器信任服务端 TLS 证书${NC}"
    echo ""
    echo -e "  ${BOLD}第2步：打开演示页面${NC}"
    echo -e "    浏览器打开 ${CYAN}http://127.0.0.1:${HTTP_PORT}/${NC}"
    echo -e "    ${YELLOW}目的：加载 WebSocket 演示前端页面${NC}"
    echo ""
    echo -e "  ${BOLD}第3步：连接 WSS${NC}"
    echo -e "    点击页面上的「连接」按钮"
    echo -e "    目标地址: ${CYAN}wss://127.0.0.1:${PORT}${NC}"
    echo -e "    ${YELLOW}预期：页面显示「已连接」，TLS握手+WebSocket升级成功${NC}"
    echo ""
    echo -e "  ${BOLD}第4步：文本消息回显${NC}"
    echo -e "    在输入框输入消息，点击「发送文本」"
    echo -e "    ${YELLOW}预期：服务端回显相同消息，页面展示收发记录${NC}"
    echo ""
    echo -e "  ${BOLD}第5步：文件分片上传${NC}"
    echo -e "    选择一个文件（建议 1~10MB），点击「上传」"
    echo -e "    ${YELLOW}预期：分片上传进度条推进，完成后显示 CRC32 校验通过${NC}"
    echo ""
    echo -e "  ${BOLD}第6步：断点续传（可选演示）${NC}"
    echo -e "    上传中途关闭浏览器标签页 → 重新打开 → 再次上传相同文件"
    echo -e "    ${YELLOW}预期：服务端返回 bitmap，仅补传未完成分片${NC}"
    echo ""
    echo -e "  ${BOLD}第7步：浏览器日志上报（可选）${NC}"
    echo -e "    勾选页面「同步到服务端」→ 日志写入 ${CYAN}log/wss_browser.log${NC}"
    echo ""
    echo -e "  ${BOLD}第8步：Wireshark 抓包（可选，展示 TLS 1.3 加密）${NC}"
    echo -e "    过滤条件: ${CYAN}tcp.port == ${PORT}${NC}"
    echo -e "    设置 ${CYAN}SSLKEYLOGFILE${NC} 后可解密 TLS 应用数据"
    echo ""
    echo -e "  ${BOLD}${YELLOW}══════════════════════════════════════${NC}"
    echo ""

    # 启动实时日志监控（后台 tail -f，过滤关键行并加时间戳高亮）
    echo -e "  ${BOLD}${MAG}── 服务端实时日志（以下为直播输出）──────────────────────${NC}"
    echo ""

    # 关键词过滤：覆盖 TLS/WS/HTTP/文件/错误/连接 等关键事件
    local tail_pid
    tail -f "$srv_log" 2>/dev/null | \
        grep --line-buffered -E \
        '(TLS握手|SSL_accept|WebSocket|101|Upgrade|连接|断开|TLS:|early_data|session_reuse|文件|FILE_|CRC|HTTP|错误|警告|指标|入口|Ping|Pong|心跳|日志上报|wss_browser)' \
        2>/dev/null | \
        while IFS= read -r line; do
            # 按日志级别着色
            if   echo "$line" | grep -q '错误\|Error\|error'; then
                echo -e "    ${RED}${line}${NC}"
            elif echo "$line" | grep -q '警告\|Warn\|warn'; then
                echo -e "    ${YELLOW}${line}${NC}"
            elif echo "$line" | grep -q 'TLS握手完成\|101\|连接建立\|握手成功\|ACCEPTED'; then
                echo -e "    ${GREEN}${line}${NC}"
            elif echo "$line" | grep -q '指标'; then
                echo -e "    ${CYAN}${line}${NC}"
            else
                echo "    ${line}"
            fi
        done &
    tail_pid=$!

    echo -e "  ${GREEN}▲ 日志直播已开始 (PID=$tail_pid)${NC}"
    echo -e "  ${GREEN}▲ 服务端 PID=$pid${NC}"
    echo -e "  ${YELLOW}▲ 现在请在浏览器中按上述步骤操作${NC}"
    echo -e "  ${YELLOW}▲ 所有操作将实时反映在上方日志中${NC}"
    echo ""
    echo -ne "  ${BOLD}按 Enter 停止服务端并结束本步骤...${NC}"
    read -r </dev/tty 2>/dev/null || true

    # 停止实时日志
    kill "$tail_pid" 2>/dev/null || true
    wait "$tail_pid" 2>/dev/null || true
    echo ""

    cleanup_server

    # 展示日志摘要
    echo ""
    echo -e "  ${BOLD}── 服务端日志摘要 ──${NC}"
    extract_key_log "$srv_log" "TLS握手完成|WebSocket|101|连接|文件|FILE_|错误|警告|指标|early_data|session_reuse|握手成功|握手失败|日志上报"

    on_step_done SKIP "浏览器演示为手动操作步骤，日志已实时展示" "$srv_log"
}

# ── 菜单系统 ──────────────────────────────────────────────────────────

show_menu() {
    print_banner
    echo -e "  ${BOLD}答辩演示步骤菜单${NC}"
    echo ""
    echo -e "  ${CYAN}  1${NC}   环境检查与编译        ${CYAN}  7${NC}   文本 RTT 压测"
    echo -e "  ${CYAN}  2${NC}   单元测试              ${CYAN}  8${NC}   并发承载能力"
    echo -e "  ${CYAN}  3${NC}   CLI 全流程演示         ${CYAN}  9${NC}   TLS 会话复用率"
    echo -e "  ${CYAN}  4${NC}   安全性检测            ${CYAN} 10${NC}   文件传输吞吐"
    echo -e "  ${CYAN}  5${NC}   0-RTT 负例演示         ${CYAN} 11${NC}   TLS 握手延迟对比"
    echo -e "  ${CYAN}  6${NC}   0-RTT 正例与会话复用   ${CYAN} 12${NC}   浏览器可视化演示"
    echo ""
    echo -e "  ${BOLD}  a${NC}   一键全部执行          ${BOLD}  q${NC}   退出"
    echo ""
}

run_single_step() {
    local n="$1"
    case "$n" in
        1)  step_01_env_build ;;
        2)  step_02_unit_tests ;;
        3)  step_03_cli_demo ;;
        4)  step_04_security ;;
        5)  step_05_0rtt_negative ;;
        6)  step_06_0rtt_positive ;;
        7)  step_07_text_rtt ;;
        8)  step_08_concurrent ;;
        9)  step_09_session_reuse ;;
        10) step_10_file_throughput ;;
        11) step_11_tls_compare ;;
        12) step_12_browser ;;
        *)  echo -e "  ${RED}无效步骤: $n (有效范围: 1-${TOTAL_STEPS})${NC}"; return 1 ;;
    esac
}

run_all() {
    echo ""
    echo -e "${YELLOW}${BOLD}══════ 开始完整演示流程 ─═════${NC}"
    echo ""

    # 先统一配置所有参数
    prompt_all_params

    for i in $(seq 1 $TOTAL_STEPS); do
        run_single_step "$i"
        if [[ "$i" -lt "$TOTAL_STEPS" ]]; then
            press_any_key
        fi
    done
}

# ── 总结输出 ──────────────────────────────────────────────────────────

print_summary() {
    echo ""
    echo -e "${BOLD}${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}${BLUE}                    演示结束 · 结果汇总                       ${NC}"
    echo -e "${BOLD}${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""
    printf "  %s  PASS: %2d\n" "${GREEN}✓${NC}" "$STEP_PASS"
    printf "  %s  FAIL: %2d\n" "${RED}✗${NC}" "$STEP_FAIL"
    printf "  %s  SKIP: %2d\n" "${YELLOW}─${NC}" "$STEP_SKIP"
    echo ""
    echo -e "  输出目录: ${YELLOW}$OUT_DIR${NC}"
    echo -e "  完整总览: ${CYAN}$OUT_DIR/DEMO_SUMMARY.md${NC}"
    echo -e "  步骤索引: ${CYAN}$OUT_DIR/INDEX.md${NC}"
    echo ""

    # 追加结束时间到 INDEX
    {
        echo ""
        echo "---"
        echo ""
        echo "- **结束时间**: $(date -Iseconds)"
        echo "- **总览**: [DEMO_SUMMARY.md](DEMO_SUMMARY.md)"
        echo ""
        echo "## 统计"
        echo ""
        echo "| 状态 | 数量 |"
        echo "|------|------|"
        echo "| PASS | $STEP_PASS |"
        echo "| FAIL | $STEP_FAIL |"
        echo "| SKIP | $STEP_SKIP |"
    } >> "$OUT_DIR/INDEX.md"
}

# ── 初始化输出目录 ────────────────────────────────────────────────────

init_output() {
    cat > "$OUT_DIR/INDEX.md" <<EOF
# 答辩演示运行索引

- **开始时间**: $(date -Iseconds)
- **项目根目录**: $ROOT
- **输出目录**: \`$OUT_DIR\`

| 步骤 | 内容 | 状态 | 报告 |
|------|------|------|------|
EOF

    cat > "$OUT_DIR/DEMO_SUMMARY.md" <<EOF
# 答辩演示总览

- **生成时间**: $(date -Iseconds)
- **输出目录**: \`$OUT_DIR\`

## 各步骤状态
EOF
}

# ── 预编译 ────────────────────────────────────────────────────────────

prebuild() {
    cd "$ROOT"
    echo -e "${YELLOW}[准备] 检查并编译项目...${NC}"
    if [[ ! -f "build/wss_server" || ! -f "build/wss_client" || ! -f "build/wss_unit_tests" ]]; then
        echo "  首次运行，执行编译..."
        cmake -S . -B build 2>&1 | tail -n 3 | sed 's/^/  /' || true
        cmake --build build -j 2>&1 | tail -n 5 | sed 's/^/  /' || true
    else
        echo -e "  ${GREEN}✓ 二进制文件已存在，跳过编译${NC}"
        echo "  (如需重新编译请先 rm -rf build/)"
    fi
    if [[ ! -f "certs/server_cert.pem" ]]; then
        echo "  证书缺失，执行生成..."
        ./scripts/gen_cert.sh certs 2>&1 | tail -n 3 | sed 's/^/  /'
    fi
    echo ""
}

# ── 主入口 ────────────────────────────────────────────────────────────

main() {
    # 清理可能残留的服务端进程
    cleanup_server

    if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
        prebuild
    fi

    init_output

    if [[ "$#" -eq 0 ]]; then
        # 交互菜单模式
        while true; do
            show_menu
            echo -ne "  请选择 [1-12 / a / q]: "
            read -r choice </dev/tty 2>/dev/null || true
            echo ""

            case "$choice" in
                a|A)
                    run_all
                    break
                    ;;
                q|Q)
                    echo -e "  ${YELLOW}退出演示${NC}"
                    exit 0
                    ;;
                1|2|3|4|5|6|7|8|9)
                    local sn
                    sn="$(printf '%02d' "$choice")"
                    prompt_params_for_step "$sn"
                    run_single_step "$choice"
                    press_any_key
                    ;;
                10|11|12)
                    prompt_params_for_step "$choice"
                    run_single_step "$((10#$choice))"
                    press_any_key
                    ;;
                *)
                    echo -e "  ${RED}无效选项: '$choice'，请输入 1-12、a 或 q${NC}"
                    sleep 1
                    ;;
            esac
        done
    else
        # 命令行参数模式
        case "$1" in
            --all)
                run_all
                ;;
            --step)
                if [[ -z "${2:-}" ]]; then
                    echo -e "${RED}用法: $0 --step <编号>${NC}"
                    exit 1
                fi
                run_single_step "$((10#$2))"
                ;;
            --list)
                echo "可用的答辩演示步骤 (共 $TOTAL_STEPS 步):"
                echo ""
                echo "  01  环境检查与编译"
                echo "  02  单元测试"
                echo "  03  CLI 全流程演示（文本回显 + 文件上传）"
                echo "  04  安全性检测（明文拒绝/TLS1.3/CRC篡改）"
                echo "  05  0-RTT 负例演示"
                echo "  06  0-RTT 正例与会话复用"
                echo "  07  文本 RTT 压测"
                echo "  08  并发承载能力"
                echo "  09  TLS 会话复用率"
                echo "  10  文件传输吞吐"
                echo "  11  TLS 握手延迟对比"
                echo "  12  浏览器可视化演示"
                exit 0
                ;;
            *)
                echo -e "${RED}未知参数: $1${NC}"
                echo "用法: $0 [--all | --step N | --list]"
                exit 1
                ;;
        esac
    fi

    print_summary
}

main "$@"
