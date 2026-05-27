# WebsocketServer · TLS 1.3 WebSocket 服务端

**Linux 上的 TLS 1.3 + WebSocket服务端参考实现**：`epoll` 非阻塞 IO、OpenSSL 非阻塞 TLS。 帧解析与 HTTP Upgrade、文本回显、二进制文件分片上传与断点续传；使用同进程 **明文 HTTP** 提供浏览器演示页。附带命令行客户端、单元测试与压测脚本。

---

## 项目概述

传统 HTTP 的请求—响应模型不适合低延迟双向推送；**WebSocket** 在单 TCP 连接上提供全双工通道。**TLS 1.3** 在传输层提供机密性与完整性，组合为 **`wss://`**。

| 层次 | 内容 |
|------|------|
| IO | `epoll`（边缘触发）、非阻塞 socket |
| 安全 | OpenSSL 3.x，仅协商 **TLS 1.3**；证书加载；Session Ticket / 超时；可选 **0-RTT 完整路径**（服务端 **`SSL_read_early_data` → `SSL_accept`**；CLI 会话 PEM + **`SSL_write_early_data`** + 升级请求 `X-Nonce`） |
| 协议 | `WebSocketCodec` + `WebSocketStreamParser`：握手 Accept、帧编解码、掩码、Ping/Pong/Close、载荷上限 |
| 并发 | **IO 线程** + **ThreadPool** 处理文本回显与文件协议；每连接 **outbound 队列** + `EPOLLOUT` 冲刷 |
| 缓冲 | **`MemoryPool`** 复用 **16KB** TLS 读缓冲，降低热路径 `vector` 分配 |
| 业务 | `FileTransferManager`：`FILE_START` / `FILE_QUERY` / `FILE_CHUNK`、bitmap、分片/整文件 **CRC32**、可选 UTF-8 文件名落盘 `uploads/` |
| 日志 | `Logger`（stderr + 可选 `log/wss_server.log` / `log/wss_client.log` / `log/wss_browser.log`）、`config/wss_server.conf` |


---

## 功能特性

- **WebSocket**：HTTP → WebSocket 升级；文本（opcode 1）与二进制（opcode 2）；Ping/Pong/Close；半包/粘包增量解析。
- **TLS 1.3**：若启用 `max_early_data`，首包 IO 为 **`SSL_read_early_data`（至 FINISH）再 `SSL_accept`**，然后 `SSL_read`；否则仅 `SSL_accept`；与 `WANT_READ` / `WANT_WRITE` 协同；可选会话复用指标日志。
- **心跳**：定时器扫描空闲连接；服务端主动 **Ping**（浏览器自动 **Pong**）；`wss_client` 支持 **`--client-ping-interval`** 主动发客户端 Ping。
- **大文件**：分片 + 分片 CRC + 整文件 CRC；**断点续传**（相同 `file_id` 且参数一致时复用 bitmap）。
- **静态页**：默认 **8080** 明文 HTTP：`GET` 映射 `web/index.html`；**浏览器日志上报** `POST /__wss_browser_log` 写入 `log_dir/wss_browser.log`（与 `wss_server.log` 分离，可 `--http-port 0` 关闭整段 HTTP）。
- **配置**：`key=value` 配置文件；**命令行 > 配置文件 > 内置默认**。

---

## 架构概览

```text
  [Listen]                         [同一 epoll 循环]
  +------------------+             +--------------------------------+
  | TLS  :8443 (WSS) |--TLS 字节-->| SSL_read / SSL_write (非阻塞)   |
  | HTTP :8080 (静态)|--HTTP GET-->| 仅映射 web/，与 WS 路径分离    |
  | timerfd          |--到期事件->| 空闲超时 / 服务端 Ping 调度    |
  +------------------+             +-----------------|--------------+
                                                    |
                                                    v
                              +-------------------------------------+
                              | WebSocketStreamParser（增量帧）      |
                              +----------+------------+-------------+
                                         |            |
                                         v            v
                              +--------------+  +-------------------+
                              | ThreadPool   |  | FileTransferMgr   |
                              | 文本回显     |  | 分片/断点/CRC     |
                              +--------------+  +-------------------+
```

- **单进程**内 **TLS + 可选 HTTP + timerfd** 共用一个 `epoll` 循环。
- **TLS 明文**喂给 WebSocket 解析器；业务结果封装成帧入队，由写路径 `SSL_write` 发出。

### 出站队列与 `eventfd` 唤醒（跨线程写路径）

- 文本回显与文件协议在 **ThreadPool** 中运行，处理完后将 WebSocket 帧放入 `Connection::outbound`，并对连接 `epoll_ctl EPOLLOUT`。
- 在 **`EPOLLET`（边沿触发）** 下，仅靠 `EPOLLOUT` 有时无法让 IO 线程立刻进入写路径；因此增加 **`eventfd`**：工作线程在入队后 **`write(wake_fd, 1)`**，`epoll_wait` 收到可读后 **排空 eventfd**，并对所有 **`tls_done==true`** 的连接调用 **`flushOutbound`**（内部 `SSL_write`）。
- **`tls_done` 为 false** 时（含 TLS 握手进行中、以及 0-RTT 下尚未结束的 **`SSL_read_early_data`**）**禁止** `SSL_write`；代码中仅在握手完成后才同步/异步 `flushOutbound`，避免 OpenSSL 致命错误。
- 源码中 **`notifyIoThreadOutbound`** / **`wake_fd`** 分支有注释；将 `Logger` 设为 **`Debug`** 可见「eventfd 唤醒」类日志。

---

## 仓库结构

```
.
├── CMakeLists.txt
├── README.md
├── .gitignore
├── config/
│   ├── README.md
│   ├── wss_server.conf
│   └── wss_server.example.conf
├── include/
│   ├── common/
│   │   ├── ConfigFile.h
│   │   ├── Crc32.h
│   │   ├── ErrorCodes.h
│   │   ├── Logger.h
│   │   ├── MemoryPool.h
│   │   └── ThreadPool.h
│   ├── file/
│   │   └── FileTransferManager.h
│   ├── server/
│   │   ├── OpenSslHelpers.h
│   │   └── WssServer.h
│   └── ws/
│       └── WebSocketCodec.h
├── src/
│   ├── common/
│   │   ├── ConfigFile.cpp
│   │   ├── Crc32.cpp
│   │   ├── Logger.cpp
│   │   ├── MemoryPool.cpp
│   │   └── ThreadPool.cpp
│   ├── client/
│   │   └── main_client.cpp
│   ├── file/
│   │   └── FileTransferManager.cpp
│   ├── server/
│   │   ├── main_server.cpp
│   │   ├── OpenSslHelpers.cpp
│   │   └── WssServer.cpp
│   └── ws/
│       └── WebSocketCodec.cpp
├── tests/
│   └── unit_tests.cpp
├── web/
│   └── index.html
├── scripts/
│   ├── gen_cert.sh
│   ├── run_demo.sh
│   ├── selftest_0rtt.sh
│   ├── test_0rtt_two_terminals.sh
│   ├── bench_text_rtt.sh
│   ├── bench_file_throughput.sh
│   ├── bench_concurrent_hold.sh
│   ├── bench_session_reuse.sh
│   ├── bench_tls_handshake_compare.sh
│   ├── bench_large_file.sh
│   ├── run_all_benches.sh
│   ├── run_valgrind.sh
│   ├── functional_smoke.sh
│   └── security_check.sh
│

```

---

## 运行环境

| 依赖 | 说明 |
|------|------|
| OS | Linux |
| 编译器 | GCC 或 Clang（**C++11**） |
| CMake | ≥ 3.16 |
| OpenSSL | **3.x**（TLS 1.3） |
| POSIX | pthread |

---

## 项目构建

### 克隆项目

```bash
git clone https://github.com/Chieko3020/WebsocketServer.git
cd WebsocketServer
```

### 安装依赖

```bash
sudo apt update
sudo apt install -y git build-essential cmake libssl-dev openssl

cmake --version    # 应 ≥ 3.16
openssl version    # 建议 OpenSSL 3.x
c++ --version || g++ --version
```

### 编译项目

```bash
cmake -S . -B build
cmake --build build -j
./build/wss_unit_tests      
./scripts/gen_cert.sh certs    # 自签名证书写入 certs/
./build/wss_server             # 在项目根目录启动
```

可选，另开终端启动客户端，也可在浏览器进行测试：

```bash
cd WebsocketServer
./build/wss_client --host 127.0.0.1 --port 8443 --ca certs/server_cert.pem --text "hello"

# 选择文件上传
./build/wss_client --host 127.0.0.1 --port 8443 --ca certs/server_cert.pem \
  --text "ok" --file ./Path/YourFile --chunk-size 16384
```

常用参数：

| 参数 | 说明 |
|------|------|
| `--enable-0rtt` | 0/1，默认 0；为 1 时升级请求带 `X-Nonce`（须与服务端 `enable_0rtt` 一致；浏览器无法带 `X-Nonce`，此时只能使用命令行客户端） |
| `--session-file <path>` | TLS 会话 PEM 路径；与 `--enable-0rtt 1` 配合时，**未指定**则默认 **`$HOME/.cache/wss_client_session.pem`**（首次连接写入，第二次连接 **`SSL_set_session` + `SSL_write_early_data`** 发送 HTTP 升级）。 |
| `--client-ping-interval <sec>` | CLI 周期性发送 **WebSocket Ping** 的间隔；**0** 表示不发送（依赖服务端 Ping）。浏览器 **无法** 主动发 RFC6455 Ping。 |
| `--log-dir <dir>` | 除 stderr 外追加写入 **`<dir>/wss_client.log`**（默认目录 `log`）。 |
| `--no-log-file` | 禁用客户端文件日志。 |

### 日志文件（`log/`）

| 文件 | 来源 |
|------|------|
| `wss_server.log` | 服务端 `Logger`（配置项 `log_dir`） |
| `wss_client.log` | 命令行客户端 `--log-dir` |
| `wss_browser.log` | 浏览器演示页勾选「同步到服务端」后，`POST http://<主机>:<http-port>/__wss_browser_log`（正文为一行文本） |


### 构建产物

| 输出路径 | 说明 |
|----------|------|
| `build/wss_server` | 服务端 |
| `build/wss_client` | 命令行客户端 |
| `build/wss_unit_tests` | 单元测试 |

---

### 配置说明

1. 默认尝试加载 **`config/wss_server.conf`**（相对于**当前工作目录**）。
2. 使用 **`--config /path/to/conf`** 指定其它文件；文件不存在且显式指定则报错退出。
3. **优先级**：**命令行参数 > 配置文件 > 代码内置默认值**。


| 键 | 含义 |
|----|------|
| `port` | WSS 监听端口 |
| `cert` / `key` | 服务端 PEM 证书与私钥 |
| `enable_ticket` | TLS Session Ticket（0/1） |
| `session_timeout` | 会话缓存超时（秒） |
| `enable_0rtt` | 0-RTT 实验（0/1，默认 0；为 1 时要求升级请求含 `X-Nonce`，**浏览器 WebSocket 无法发送**，演示页请保持 0） |
| `http_port` / `http_root` | 内置静态 HTTP；`http_port=0` 关闭 |
| `log_dir` / `log_file` | 文件日志目录与是否写入 `wss_server.log` |
| `ws_idle_timeout` / `ws_ping_interval` | WebSocket 空闲秒数与服务端 Ping 间隔（0 表示自动） |

---

### 浏览器演示

1. 打开 **`http://127.0.0.1:8080/`**（默认 HTTP 端口，可按配置修改）。
2. 按页面说明在浏览器中**信任自签名证书**（通常需先访问一次 `https://127.0.0.1:8443` 或页面内链接）。
3. 在页面中连接 **`wss://127.0.0.1:8443`**，进行文本回显与文件上传。

**注意**：地址栏直接打开 `https://127.0.0.1:8443` 是普通 HTTPS 页面请求，**不会**自动携带 WebSocket `Upgrade`，服务端日志可能出现非 WS 数据提示，属预期现象。

### 0-RTT 测试

在**两个终端**中**分别**启动服务端与客户端

**1. 证书**

```bash
./scripts/gen_cert.sh certs
```

**2. 终端 A — 服务端**：开启 0-RTT 
```bash
./build/wss_server --enable-0rtt 1 --port 8443 \
  --cert certs/server_cert.pem --key certs/server_key.pem \
  --http-port 8080 --http-root web
```

也可在 **`config/wss_server.conf`** 中设置 `enable_0rtt=1` 后，直接 **`./build/wss_server`**（仍须在根目录启动以加载 `config/`、`web/`）。

**3. 终端 B — 客户端（第一次：建立会话并落盘）**：必须 **`--enable-0rtt 1`**，否则升级请求不带 `X-Nonce`，服务端会拒绝。可选 **`--session-file /tmp/wss_sess.pem`**；若不指定，默认写入 **`$HOME/.cache/wss_client_session.pem`**（请确保父目录可写或先 `mkdir -p ~/.cache`）。

```bash
./build/wss_client --host 127.0.0.1 --port 8443 --ca certs/server_cert.pem \
  --text "hello" --enable-0rtt 1 --session-file /tmp/wss_sess.pem
```

**4. 第二次连接（完整 TLS 1.3 early data 路径）**：**同一命令**再执行一次（会话 PEM 已存在）。客户端日志中应出现类似 **`early_data_status=ACCEPTED`**（或 **`REJECTED`** / **`NOT_SENT`**，与 OpenSSL 反重放、服务端策略有关），并注明 **`HTTP 升级已通过 early data 发送`**；**`TLS_session_reused=1`**、**`try_SSL_write_early_data=1`**。服务端同连接日志中 **`session_reused=1`**、**`early_data_status=ACCEPTED`**。第一次连接成功后应看到 **`会话吸收后 max_early_data=16384`**（客户端在发 HTTP 前吸收 post-handshake NewSessionTicket，否则 PEM 里 `Max Early Data` 常为 0）。

**5. 使用脚本**：在项目根目录执行 **`./scripts/test_0rtt_two_terminals.sh`**（默认 WSS 端口 **`18555`**，可用 **`WSS_TEST_PORT`** / **`WSS_TEST_SESS`** 覆盖）。脚本内用前缀 **`[A]`** / **`[B1]`** / **`[B2]`** 区分「服务端 / 第一次客户端 / 第二次客户端」日志；**真实手测仍建议两个终端分别运行**，以免与脚本管道缓冲混淆。

**说明**：浏览器 **`WebSocket` 无法发送 `X-Nonce`**，不走 CLI 的 early data 写路径；浏览器演示请保持 **`enable_0rtt=0`**。

#### 0-RTT 指标

| 维度 | 判据（正面/说明） |
|------|-------------------|
| **应用层 0-RTT 模式** | 服务端日志出现 **`TLS:0-RTT实验开关：开启`**；客户端为 **`--enable-0rtt 1`** 时，**不出现** `Missing X-Nonce`，且出现 **`HTTP升级完成，发送101`**。 |
| **负例对照** | 仅服务端 `enable_0rtt=1`、客户端**未**加 `--enable-0rtt 1` 时，应出现 **`Missing X-Nonce in 0-RTT mode`**，TLS 仍成功但 **无 101**（证明 `X-Nonce` 与开关绑定）。 |
| **业务闭环** | 客户端 **`WebSocket 握手成功`**、**文本回显**正常。 |
| **周期性指标**（stderr / `log/wss_server.log` 中「指标」行） | **`握手成功`** 递增、**`握手失败`** 不异常堆积；**`新握手`** 首次连接通常会增加；**`复用握手`** 在 Session Ticket 复用后可能增加（用于佐证会话复用，非严格等价 TLS early data 字节）。 |
| **TLS early data 日志** | 服务端 / 客户端在握手完成后输出 **`early_data_status=`** + **`ACCEPTED` / `REJECTED` / `NOT_SENT`**；**首次连接**无会话文件时多为 **`NOT_SENT`**；**第二次 CLI 连接**在会话复用成功时客户端可出现 **`ACCEPTED`**。 |


## 辅助脚本

| 脚本 | 作用 |
|------|------|
| `scripts/gen_cert.sh` | 生成 `server_cert.pem` / `server_key.pem` |
| `scripts/run_demo.sh` | 服务端 + 客户端演示 |
| `scripts/selftest_0rtt.sh` | 0-RTT / `X-Nonce` 0-RTT 测试脚本 |
| `scripts/test_0rtt_two_terminals.sh` | 顺序跑服务端 + 两次客户端，带 `[A]/[B1]/[B2]` 日志前缀（默认端口 18555） |
| `scripts/bench_text_rtt.sh` | 文本 RTT 压测，输出 `bench_output/*.csv` |
| `scripts/bench_file_throughput.sh` | 文件吞吐压测 |
| `scripts/bench_concurrent_hold.sh` | 并发承载能力压测（200/500/1000 连接） |
| `scripts/bench_session_reuse.sh` | TLS 会话复用率压测 |
| `scripts/bench_tls_handshake_compare.sh` | TLS 1.3 vs TLS 1.2 握手延迟对比 |
| `scripts/bench_large_file.sh` | 大文件（10/100MB）传输成功率压测 |
| `scripts/run_all_benches.sh` | 一键运行全部压测脚本 |
| `scripts/run_valgrind.sh` | Valgrind 内存泄漏检测 |
| `scripts/functional_smoke.sh` | 功能冒烟测试 |
| `scripts/security_check.sh` | 安全测试（明文 TLS、错误握手、篡改 CRC） |

---

## 协议与端口

**默认（可在配置中修改）**

| 端口 | 协议 | 说明 |
|------|------|------|
| 8443 | TLS + WebSocket | `wss://` 业务 |
| 8080 | HTTP/1.1 | 仅静态文件；`GET /` → `index.html` |

**应用层文件协议（WebSocket 二进制帧 payload）**简述：

- `FILE_START`：`file_id`、大小、分片参数、整文件 CRC32、可选 UTF-8 文件名。
- `FILE_QUERY` / `FILE_QUERY_RESPONSE`：bitmap。
- `FILE_CHUNK`：分片数据 + 分片 CRC32。
- 完成：`FILE_FINISH_ACK`；错误：`FILE_ERROR` + 统一错误码（见 `include/common/ErrorCodes.h`）。

---

## 测试

```bash
./build/wss_unit_tests
```

覆盖 WebSocket Accept、CRC32、掩码解析、`ConfigFile`、**`MemoryPool`** 等基础逻辑。功能与性能实验可使用 `scripts/bench_*.sh`，结果默认在 **`bench_output/`**。

---

## 常见问题

| 现象 | 说明 |
|------|------|
| 连接约空闲一段时间后断开 | 服务端有空闲超时与服务端 Ping；可调大 `ws_idle_timeout` 或检查 Ping 间隔。 |
| 浏览器 `1006` / `Missing X-Nonce` | 服务端 **`enable_0rtt=1`** 时要求升级请求带 **`X-Nonce`**；**浏览器 WebSocket 无法发送自定义头**，与证书是否信任无关。请 **`enable_0rtt=0`** 后再连；0-RTT 实验请用 **`wss_client --enable-0rtt 1`**。其它 `1006` 多为服务端直接断 TCP，结合日志排查。 |
| `Missing Sec-WebSocket-Key` | 多为在 8443 上打开了普通 **HTTPS 页面**（用于信任证书），不是 WebSocket，可忽略；或检查是否误用非 WS 客户端。 |
| `TLS握手失败` / 首条连接即失败 | 常见原因：**用明文 HTTP 访问了 TLS 端口**（例如浏览器打开 `http://127.0.0.1:8443`、或健康检查打错端口）；8443 只接受 TLS 记录。请用 **`http://127.0.0.1:<http_port>`** 访问静态页，**`wss://`** 走 8443。另：并发探测、证书不信任也可能出现；偶发可忽略，持续失败再查证书与端口。 |
| 浏览器日志未写入 `wss_browser.log` | 演示页「同步到服务端」使用 **`POST http://<主机>:<http-port>/__wss_browser_log`**，必须走 **明文 HTTP 端口**（默认 8080），**不能**发到 `wss://8443`。 |
| 池化缓冲 WARN | TLS 读 **MemoryPool** 瞬时借尽时会堆回退并打 WARN，通常见于极高并发。 |

---