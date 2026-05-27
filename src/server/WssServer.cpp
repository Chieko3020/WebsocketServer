#include "server/WssServer.h"

#include "common/Logger.h"
#include "common/MemoryPool.h"
#include "common/ErrorCodes.h"
#include "common/ThreadPool.h"
#include "file/FileTransferManager.h"
#include "server/OpenSslHelpers.h"
#include "ws/WebSocketCodec.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>

/*
 * 文件作用说明：
 * 该文件是服务端主流程实现文件，承担如下职责：
 * 1) 初始化 TLS1.3 上下文；
 * 2) 创建监听 socket 与 epoll 事件循环；
 * 3) 处理连接建立、TLS 非阻塞握手、WebSocket 帧解析；
 * 4) 处理控制帧（Ping/Pong/Close）与业务帧（文本/二进制）；
 * 5) 协同线程池和文件传输模块完成业务逻辑；
 * 6) 执行心跳超时清理与连接资源回收；
 * 7) 可选：在同一 epoll 上提供明文 HTTP 静态页（如 web/index.html），与 WSS 监听端口分离。
 * - IO 线程负责网络读写与协议边界；
 * - 业务线程仅做消息处理，不直接做 socket 阻塞操作。
 */

namespace {

static bool isValidCloseCode(uint16_t code) {
  // RFC6455 常见合法区间：1000-4999，排除保留/未定义值的一部分。
  if (code < 1000 || code >= 5000) return false;
  if (code == 1004 || code == 1005 || code == 1006 || code == 1015) return false;
  return true;
}

static int setNonBlocking(int fd) {
  // 函数：把文件描述符切换为非阻塞模式，成功返回0，失败返回-1。
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint32_t baseInterest() {
  // Edge-triggered + half-close detection.
  return static_cast<uint32_t>(EPOLLET | EPOLLRDHUP);
}

// —— 浏览器演示页：仅通过「明文 HTTP 端口」POST 一行文本到 /__wss_browser_log，落盘为 wss_browser.log（与 Logger 的 wss_server.log 分离）——
static std::mutex g_browser_log_file_mu;  // 保护多连接同时 POST 时对同一文件的顺序写入，避免行交错

// 确保 log_dir 存在：已是目录则成功；否则 mkdir(0755)；若两线程同时创建遇 EEXIST 也视为成功。
static bool ensureDirForLog(const std::string& d) {
  struct stat st;  // struct 关键字不可省：避免与 ::stat() 函数名冲突
  if (::stat(d.c_str(), &st) == 0) return S_ISDIR(st.st_mode);  // 路径存在：必须是目录才继续
  if (::mkdir(d.c_str(), 0755) == 0) return true;  // 新建目录成功
  return errno == EEXIST;  // 竞态：他线程已创建同名路径
}

// 时间格式与 Logger::nowString 一致，便于把浏览器行与服务器模块日志按时间 grep 对齐。
static std::string httpLogTimestamp() {
  const auto now = std::chrono::system_clock::now();  // wall clock
  const std::time_t t = std::chrono::system_clock::to_time_t(now);  // 秒精度
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;  // 毫秒三位
  std::tm tm_buf;  // 本地时区分解
  localtime_r(&t, &tm_buf);  // 线程安全 localtime
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << "." << std::setw(3) << std::setfill('0') << ms.count();
  return oss.str();  // 例：2026-03-22 12:00:00.123
}

// 追加一行：浏览器 POST 的正文（通常一行）写入 log_dir/wss_browser.log；失败时返回 false（上层可 503 或 WARN）。
static bool appendBrowserClientLogFile(const std::string& log_dir, const std::string& one_line) {
  if (log_dir.empty()) return false;  // run() 传空则关闭浏览器日志端点（见 driveHttpRead 503 分支）
  if (!ensureDirForLog(log_dir)) return false;  // 无法保证目录存在则拒绝写入
  std::lock_guard<std::mutex> lk(g_browser_log_file_mu);  // 与 Logger 的全局锁独立，仅串行化 browser 文件
  const std::string path = log_dir + "/wss_browser.log";  // 固定文件名，与 README 表格一致
  std::ofstream ofs(path.c_str(), std::ios::app | std::ios::out);  // 追加，多进程未协调（单进程服务足够）
  if (!ofs.good()) return false;  // 磁盘满、权限等
  // 伪 Logger 行：[时间][信息] wss_browser:用户正文\n
  ofs << "[" << httpLogTimestamp() << "][信息] wss_browser:" << one_line << "\n";
  ofs.flush();  // 便于 tail -f 立即看到
  LOG_DEBUG("HTTP静态", "已写入 wss_browser.log 一行，长度=" + std::to_string(one_line.size()));
  return true;
}

struct OutboundItem {
  std::vector<uint8_t> data;
  std::size_t offset{0};
};

struct ServerMetrics {
  // 发送队列最大长度（观测瞬时背压）。
  std::atomic<uint64_t> tx_queue_peak{0};
  // 真实发送字节累计（基于 SSL_write 成功返回值）。
  std::atomic<uint64_t> tx_bytes_total{0};
  // 握手结果统计。
  std::atomic<uint64_t> handshake_ok{0};
  std::atomic<uint64_t> handshake_fail{0};
  // 会话复用统计：用于评估 resumption 效果。
  std::atomic<uint64_t> handshake_reused{0};
  std::atomic<uint64_t> handshake_new{0};
};

static void updatePeak(std::atomic<uint64_t>* peak, uint64_t value) {
  // 原子更新“峰值”，避免多线程竞态下峰值丢失。
  uint64_t oldv = peak->load(std::memory_order_relaxed);
  while (value > oldv && !peak->compare_exchange_weak(oldv, value, std::memory_order_relaxed)) {
  }
}

struct Connection {
  // 底层连接 fd，对应一个客户端 TCP 连接。
  int fd{-1};
  // OpenSSL 会话对象，承载 TLS 状态机。
  SSL* ssl{nullptr};
  // 自增连接编号，主要用于日志与业务关联。
  uint64_t id{0};
  // 标识 TLS 握手是否已经完成（可安全使用 SSL_read）。
  bool tls_done{false};
  // OpenSSL 3.x 要求：若启用 max_early_data，则首包 IO 须为 SSL_read_early_data，直至 FINISH 后再 SSL_accept。
  // 若 ctx 未启用 early data，此项在构造时置 true，跳过 early 读阶段。
  bool tls_early_read_finished{false};
  // 每个连接独立维护一个 WebSocket 增量解析器。
  WebSocketStreamParser ws;
  // 是否处于“等待发送 close 帧后关闭”状态。
  bool closing{false};
  // HTTP Upgrade 已完成（已进入 WebSocket 帧阶段），此前不可发 WS Ping。
  bool ws_upgraded{false};

  // 待发送队列：业务线程投递，IO 线程发送。
  std::deque<OutboundItem> outbound;
  std::mutex outbound_mu;

  // 最近一次接收数据时间，可用于扩展空闲连接淘汰策略。
  std::chrono::steady_clock::time_point last_activity;
  // 最近一次“客户端保活”时间：收到 Ping/Pong 或任意文本/二进制数据时更新；
  // 服务端主动发送 Ping 时也会刷新，避免浏览器无法发 Ping 时被误杀。
  std::chrono::steady_clock::time_point last_ping;
  // 上次向客户端发送服务端 Ping 的时间（用于按间隔发心跳）。
  std::chrono::steady_clock::time_point last_server_ping_sent;

  explicit Connection(int cfd, SSL* s, uint64_t cid)
      : fd(cfd),
        ssl(s),
        id(cid),
        last_activity(std::chrono::steady_clock::now()),
        last_ping(std::chrono::steady_clock::now()),
        last_server_ping_sent(std::chrono::steady_clock::now()) {}

  ~Connection() {
    // 延迟到析构再释放 SSL/fd：业务线程可能仍持有本连接的 shared_ptr。
    if (ssl) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
      ssl = nullptr;
    }
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
};

static void closeConnection(int epoll_fd, std::unordered_map<int, std::shared_ptr<Connection>>& conns,
                             int fd) {
  // 函数：统一连接关闭流程（从epoll移除、TLS关闭、fd关闭、连接表回收）。
  auto it = conns.find(fd);
  if (it == conns.end()) return;

  auto c = it->second;
  {
    std::lock_guard<std::mutex> lk(c->outbound_mu);
    c->closing = true;
  }
  conns.erase(it);

  epoll_event ev;
  std::memset(&ev, 0, sizeof(ev));
  epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev);

  if (c->fd >= 0) {
    ::close(c->fd);
    c->fd = -1;
  }
  // 不在此处 SSL_free：线程池任务可能仍持有 shared_ptr<Connection>。
  LOG_INFO("服务器", "连接已关闭，fd=" + std::to_string(fd) + "，连接ID=" + std::to_string(c->id));
}

static void updateInterest(int epoll_fd, int fd, bool want_write) {
  if (fd < 0) return;
  // 函数：动态更新某连接在 epoll 中的关注事件。
  epoll_event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.data.fd = fd;
  ev.events = baseInterest() | EPOLLIN;
  if (want_write) ev.events |= EPOLLOUT;
  epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

// flushOutbound 定义在下方；processWsInboundBuffer 在 IO 线程内需要同步调用以立刻发出 101/Pong 等。
static bool flushOutbound(int epoll_fd, std::shared_ptr<Connection> c, ServerMetrics* metrics);

// 线程池线程在入队 outbound 后调用：跨线程 epoll_ctl EPOLLOUT 在 ET 模式下未必立即唤醒 epoll_wait，
// 用 eventfd 唤醒 IO 线程并在本线程外执行 flushOutbound（见 run() 主循环中对 wake_fd 的分支）。
static void notifyIoThreadOutbound(int wake_fd) {
  if (wake_fd < 0) return;
  uint64_t one = 1;
  if (::write(wake_fd, &one, sizeof(one)) < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      LOG_WARN("服务器", "eventfd 唤醒失败 errno=" + std::to_string(errno));
    }
  }
}

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
// 将握手结束后的 SSL_get_early_data_status 枚举映射为日志字符串（与 main_client earlyDataStatusLabelClient 对应）。
static const char* earlyDataStatusLabel(const SSL* ssl) {
  switch (SSL_get_early_data_status(ssl)) {  // 握手完成后查询 0-RTT 是否被服务端接受
    case SSL_EARLY_DATA_ACCEPTED:
      return "ACCEPTED";
    case SSL_EARLY_DATA_REJECTED:
      return "REJECTED";
    case SSL_EARLY_DATA_NOT_SENT:
      return "NOT_SENT";
    default:
      return "UNKNOWN";
  }
}

// 在 SSL_accept 返回 1 后调用：打印 early_data 与 session 复用，便于与 CLI 第二次连接对照实验。
static void logTlsEarlyDataProbe(SSL* ssl, uint64_t connId) {
  LOG_INFO("TLS", std::string("连接ID=") + std::to_string(connId) +
                     " early_data_status=" + earlyDataStatusLabel(ssl) +
                     " session_reused=" + (SSL_session_reused(ssl) ? "1" : "0"));
}
#else
static void logTlsEarlyDataProbe(SSL* /*ssl*/, uint64_t /*connId*/) {}
#endif

// 处理 TLS 应用数据（与 SSL_read / SSL_read_early_data 共用）：喂入 WebSocket 解析器。
// 返回 false 表示已关闭连接；stop_read_burst 为 true 时应退出 SSL_read 内层循环。
static bool processWsInboundBuffer(int epoll_fd, std::unordered_map<int, std::shared_ptr<Connection>>& conns,
                                   int fd, std::shared_ptr<Connection> c, ServerMetrics* metrics, ThreadPool& pool,
                                   FileTransferManager& fileManager, int wake_fd, const uint8_t* buf, int ret,
                                   bool* stop_read_burst) {
  *stop_read_burst = false;
  c->last_activity = std::chrono::steady_clock::now();
  std::string acceptResp;
  std::vector<WsFrame> frames;
  try {
    (void)c->ws.feed(buf, static_cast<std::size_t>(ret), &acceptResp, &frames);
  } catch (const std::exception& ex) {
    LOG_ERROR("WebSocket", "连接ID=" + std::to_string(c->id) + " 帧解析失败：" + ex.what());
    closeConnection(epoll_fd, conns, fd);
    return false;
  }

  if (!acceptResp.empty()) {  // WebSocketCodec 已生成完整 101 响应字节串
    LOG_INFO("WebSocket", "连接ID=" + std::to_string(c->id) + " HTTP升级完成，发送101响应");
    c->ws_upgraded = true;  // 之后定时器才能发 RFC6455 Ping（握手前发 Ping 非法）
    c->last_server_ping_sent = std::chrono::steady_clock::now();  // 作为服务端 Ping 间隔基准
    std::vector<uint8_t> respBytes(acceptResp.begin(), acceptResp.end());  // string → 字节队列
    {
      std::lock_guard<std::mutex> lk(c->outbound_mu);  // 与线程池入队共用同一把锁
      OutboundItem item;
      item.data = std::move(respBytes);
      item.offset = 0;  // 尚未通过 SSL_write 发出任何字节
      c->outbound.push_back(std::move(item));
      updatePeak(&metrics->tx_queue_peak, static_cast<uint64_t>(c->outbound.size()));
    }
    updateInterest(epoll_fd, fd, true);  // 需要 EPOLLOUT 才能 SSL_write
    // 关键：0-RTT 时 HTTP Upgrade 在 SSL_read_early_data 路径解析，此时 tls_done 仍为 false，禁止 SSL_write(101)；
    // 若此处 flush，OpenSSL 会处于错误状态。握手完成 tls_done=true 后由 epoll 分支或 wake 统一 flush。
    if (c->tls_done) (void)flushOutbound(epoll_fd, c, metrics);
  }

  for (const auto& frame : frames) {
    if (frame.opcode == 0x9) {
      c->last_ping = std::chrono::steady_clock::now();
      LOG_DEBUG("心跳", "连接ID=" + std::to_string(c->id) + " 收到Ping，准备回复Pong");
      auto pong = WebSocketCodec::buildPong(frame.payload);
      {
        std::lock_guard<std::mutex> lk(c->outbound_mu);
        OutboundItem item;
        item.data = std::move(pong);
        item.offset = 0;
        c->outbound.push_back(std::move(item));
        updatePeak(&metrics->tx_queue_peak, static_cast<uint64_t>(c->outbound.size()));
      }
      updateInterest(epoll_fd, fd, true);
      if (c->tls_done) (void)flushOutbound(epoll_fd, c, metrics);
      continue;
    }
    if (frame.opcode == 0xA) {
      c->last_ping = std::chrono::steady_clock::now();
      LOG_DEBUG("心跳", "连接ID=" + std::to_string(c->id) + " 收到Pong");
      continue;
    }
    if (frame.opcode == 0x8) {
      c->closing = true;
      LOG_INFO("WebSocket", "连接ID=" + std::to_string(c->id) + " 收到Close帧，进入关闭流程");
      uint16_t code = 1000;
      if (frame.payload.size() >= 2) {
        code = static_cast<uint16_t>((frame.payload[0] << 8) | frame.payload[1]);
        if (!isValidCloseCode(code)) {
          LOG_WARN("WebSocket", "连接ID=" + std::to_string(c->id) + " 收到非法Close码=" + std::to_string(code) +
                                    "，改用1002");
          code = 1002;
        }
      }
      auto closeResp = WebSocketCodec::buildClose(code);
      {
        std::lock_guard<std::mutex> lk(c->outbound_mu);
        OutboundItem item;
        item.data = std::move(closeResp);
        item.offset = 0;
        c->outbound.push_back(std::move(item));
        updatePeak(&metrics->tx_queue_peak, static_cast<uint64_t>(c->outbound.size()));
      }
      updateInterest(epoll_fd, fd, true);
      if (c->tls_done) (void)flushOutbound(epoll_fd, c, metrics);
      *stop_read_burst = true;
      return true;
    }

    if (frame.opcode == 0x1) {
      c->last_ping = std::chrono::steady_clock::now();
      auto payloadCopy = frame.payload;
      std::weak_ptr<Connection> weak = c;
      pool.submit([weak, payloadCopy, wake_fd, epoll_fd, metrics]() mutable {
        try {
          auto conn = weak.lock();
          if (!conn) return;
          std::string text(payloadCopy.begin(), payloadCopy.end());
          LOG_DEBUG("业务", "连接ID=" + std::to_string(conn->id) + " 文本消息长度=" + std::to_string(text.size()));
          auto respFrame = WebSocketCodec::buildTextFrame(text);
          bool notify = false;
          {
            std::lock_guard<std::mutex> lk(conn->outbound_mu);
            if (conn->closing || conn->fd < 0 || !conn->ssl) return;
            OutboundItem item;
            item.data = std::move(respFrame);
            item.offset = 0;
            conn->outbound.push_back(std::move(item));
            updatePeak(&metrics->tx_queue_peak, static_cast<uint64_t>(conn->outbound.size()));
            notify = true;
          }
          if (notify) {
            updateInterest(epoll_fd, conn->fd, true);
            notifyIoThreadOutbound(wake_fd);
          }
        } catch (...) {
          LOG_WARN("业务", "文本消息处理异常（已忽略）");
        }
      });
    } else if (frame.opcode == 0x2) {
      c->last_ping = std::chrono::steady_clock::now();
      auto payloadCopy = frame.payload;
      std::weak_ptr<Connection> weak = c;
      pool.submit([weak, payloadCopy, wake_fd, epoll_fd, metrics, &fileManager]() mutable {
        try {
          auto conn = weak.lock();
          if (!conn) return;
          std::vector<std::vector<uint8_t>> replies;
          fileManager.handleClientMessage(conn->id, payloadCopy, &replies);
          if (replies.empty()) return;
          LOG_DEBUG("文件", "连接ID=" + std::to_string(conn->id) +
                                " 生成文件协议响应数量=" + std::to_string(replies.size()));
          bool notify = false;
          {
            std::lock_guard<std::mutex> lk(conn->outbound_mu);
            if (conn->closing || conn->fd < 0 || !conn->ssl) return;
            for (auto& r : replies) {
              auto respFrame = WebSocketCodec::buildBinaryFrame(r);
              OutboundItem item;
              item.data = std::move(respFrame);
              item.offset = 0;
              conn->outbound.push_back(std::move(item));
            }
            updatePeak(&metrics->tx_queue_peak, static_cast<uint64_t>(conn->outbound.size()));
            notify = true;
          }
          if (notify) {
            updateInterest(epoll_fd, conn->fd, true);
            notifyIoThreadOutbound(wake_fd);
          }
        } catch (...) {
          LOG_WARN("文件", "文件消息处理异常（已忽略）");
        }
      });
    } else {
      std::string errText = error_codes::buildTextError(error_codes::Code::UNSUPPORTED_OPCODE,
                                                        "unsupported websocket opcode=" + std::to_string(frame.opcode));
      auto errFrame = WebSocketCodec::buildTextFrame(errText);
      {
        std::lock_guard<std::mutex> lk(c->outbound_mu);
        OutboundItem item;
        item.data = std::move(errFrame);
        item.offset = 0;
        c->outbound.push_back(std::move(item));
      }
      updateInterest(epoll_fd, fd, true);
      if (c->tls_done) (void)flushOutbound(epoll_fd, c, metrics);
      LOG_WARN("WebSocket", "连接ID=" + std::to_string(c->id) + " 收到未支持opcode=" + std::to_string(frame.opcode));
    }
  }

  if (c->closing) *stop_read_burst = true;
  return true;
}

// —— TLS 1.3 0-RTT：服务端必须先读 early application data，再 SSL_accept（OpenSSL 3.x 与 max_early_data 配套）——
// 若 ctx 上 max_early_data>0，则本连接第一个「读应用数据」的 API 必须是 SSL_read_early_data，循环直到
// SSL_READ_EARLY_DATA_FINISH，其间解密出的明文即客户端 0-RTT 阶段发出的字节（本项目为带 X-Nonce 的 HTTP Upgrade）。
// 返回值：1 = early 阶段结束，可调用 SSL_accept；0 = 需等待 socket IO（epoll 已更新）；-1 = 协议/配置错误，关连接。
static int driveTlsEarlyRead(int epoll_fd, std::unordered_map<int, std::shared_ptr<Connection>>& conns,
                             std::shared_ptr<Connection> c, ServerMetrics* metrics, MemoryPool* tlsReadPool,
                             ThreadPool& pool, FileTransferManager& fileManager, int wake_fd) {
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
  while (true) {  // 同一 epoll 事件内尽量读尽 early 记录，减少 epoll_wait 往返
    TlsReadBufferGuard bufGuard(tlsReadPool);  // 析构时 release 缓冲回池
    uint8_t* buf = bufGuard.data();  // TLS 明文输出缓冲区
    const int bufLen = static_cast<int>(bufGuard.size());  // 与 SSL_read 路径相同 16KB
    size_t readbytes = 0;  // 出参：本次成功解密出的 early 明文字节数（可为 0）
    int ed = SSL_read_early_data(c->ssl, buf, static_cast<size_t>(bufLen), &readbytes);  // OpenSSL 3 签名：size_t* readbytes
    if (ed == SSL_READ_EARLY_DATA_SUCCESS) {  // 本调用已处理部分 early 流
      if (readbytes > 0) {  // 有明文才喂解析器（readbytes==0 仅表示 TLS 层内部推进）
        bool stop = false;  // 解析器请求停止 SSL_read_early_data 内层循环（如收到 Close）
        if (!processWsInboundBuffer(epoll_fd, conns, c->fd, c, metrics, pool, fileManager, wake_fd, buf,
                                    static_cast<int>(readbytes), &stop)) {
          return -1;  // processWs 已 closeConnection
        }
        if (stop) return 1;  // 与 FINISH 等价：可进入 accept（异常情况早停）
      }
      continue;  // 可能同一 TCP 读事件里还有后续 early 记录，继续 SSL_read_early_data
    }
    if (ed == SSL_READ_EARLY_DATA_FINISH) {  // 所有 early data 已从栈上消费完
      return 1;  // 接下来 advanceTlsHandshake 将置 tls_early_read_finished 并调用 SSL_accept
    }
    {
      int err = SSL_get_error(c->ssl, 0);  // ed 为错误码时 SSL_get_error 用 0（OpenSSL 文档约定）
      if (err == SSL_ERROR_WANT_READ) {  // 需更多入站 TLS 记录
        updateInterest(epoll_fd, c->fd, false);  // 只关心读（不写）
        return 0;  // 挂起，等下次 EPOLLIN
      }
      if (err == SSL_ERROR_WANT_WRITE) {  // early 读阶段也可能要出站（如 HelloRetry 类交互，少见）
        updateInterest(epoll_fd, c->fd, true);  // 打开 EPOLLOUT
        return 0;
      }
      LOG_ERROR("TLS", "连接ID=" + std::to_string(c->id) + " SSL_read_early_data 失败，err=" + std::to_string(err));
      return -1;  // 如协议错、密钥错等
    }
  }
#else
  (void)epoll_fd;  // 旧版 OpenSSL 无 early API：直接视为「无 early 阶段」
  (void)conns;
  (void)c;
  (void)metrics;
  (void)tlsReadPool;
  (void)pool;
  (void)fileManager;
  (void)wake_fd;
  return 1;  // 与「已 FINISH」同义，advanceTlsHandshake 将只跑 SSL_accept
#endif
}

// —— TLS 1.3 握手后半段：在 early 读结束后调用 SSL_accept，完成加密握手与证书验证（服务端角色）——
// 返回：1=握手成功；0=WANT_READ/WRITE；其它=-1。
static int driveTlsAccept(int epoll_fd, std::shared_ptr<Connection> c, ServerMetrics* metrics) {
  while (true) {  // 非阻塞：直到 1 或 WANT_* 或硬错误
    int ret = SSL_accept(c->ssl);  // 推进服务端握手状态机
    if (ret == 1) {  // 握手完成，可 SSL_read/SSL_write 应用数据
      logTlsEarlyDataProbe(c->ssl, c->id);  // 条件编译：打 early_data_status / session_reused
      metrics->handshake_ok.fetch_add(1, std::memory_order_relaxed);  // 指标：成功次数
      if (SSL_session_reused(c->ssl))
        metrics->handshake_reused.fetch_add(1, std::memory_order_relaxed);  // ticket/会话复用
      else
        metrics->handshake_new.fetch_add(1, std::memory_order_relaxed);  // 完整握手
      c->tls_done = true;  // 此后才允许 flushOutbound 使用 SSL_write（0-RTT 阶段严禁写）
      updateInterest(epoll_fd, c->fd, false);  // 默认先只读应用数据；有 outbound 再 MOD 加 EPOLLOUT
      LOG_INFO("TLS", "连接ID=" + std::to_string(c->id) + " TLS握手完成（SSL_accept）");
      return 1;
    }

    int err = SSL_get_error(c->ssl, ret);  // ret<=0 时必须取错误码
    if (err == SSL_ERROR_WANT_READ) {
      updateInterest(epoll_fd, c->fd, false);
      return 0;  // 保持连接，等可读
    }
    if (err == SSL_ERROR_WANT_WRITE) {
      updateInterest(epoll_fd, c->fd, true);
      return 0;  // 等可写继续 accept
    }

    LOG_ERROR("TLS", "连接ID=" + std::to_string(c->id) + " TLS握手失败，错误码=" + std::to_string(err));
    metrics->handshake_fail.fetch_add(1, std::memory_order_relaxed);
    return -1;  // 如明文打到 TLS 端口、证书错等；常见 err=1 SSL_ERROR_SSL
  }
}

// 握手总入口：(1) 若需 early 则 driveTlsEarlyRead 直至 FINISH；(2) 再 driveTlsAccept 直至 tls_done。
static int advanceTlsHandshake(int epoll_fd, std::unordered_map<int, std::shared_ptr<Connection>>& conns,
                               std::shared_ptr<Connection> c, ServerMetrics* metrics, MemoryPool* tlsReadPool,
                               ThreadPool& pool, FileTransferManager& fileManager, int wake_fd) {
  if (c->tls_done) return 1;  // 已握手，直接返回（调用方应走 SSL_read 应用路径）
  if (!c->tls_early_read_finished) {  // 尚未读完 early（或 max_early_data==0 时在 accept 前置 true）
    int er = driveTlsEarlyRead(epoll_fd, conns, c, metrics, tlsReadPool, pool, fileManager, wake_fd);
    if (er < 0) return -1;  // early 阶段失败
    if (er == 0) return 0;  // 需更多 IO，留在 epoll
    c->tls_early_read_finished = true;  // FINISH 或等价跳过，之后只接受 SSL_accept
  }
  if (!c->tls_done) {  // early 结束后若仍未完成握手
    return driveTlsAccept(epoll_fd, c, metrics);  // 可能 1/0/-1
  }
  return 1;
}

// Returns true if outbound queue is fully drained.
static bool flushOutbound(int epoll_fd, std::shared_ptr<Connection> c, ServerMetrics* metrics) {
  // 函数：刷新发送队列，尽量把待发送数据通过 SSL_write 发出。
  if (c->fd < 0 || !c->ssl) return true;
  std::unique_lock<std::mutex> lk(c->outbound_mu);
  if (c->closing && c->outbound.empty()) {
    updateInterest(epoll_fd, c->fd, false);
    return true;
  }
  if (c->outbound.empty()) {
    // 队列为空时，主动关闭写关注，避免空转触发 EPOLLOUT。
    updateInterest(epoll_fd, c->fd, false);
    return true;
  }

  while (!c->outbound.empty()) {
    OutboundItem& item = c->outbound.front();
    const uint8_t* ptr = item.data.data() + item.offset;
    std::size_t remaining = item.data.size() - item.offset;
    if (remaining == 0) {
      c->outbound.pop_front();
      continue;
    }

    int ret = SSL_write(c->ssl, ptr, static_cast<int>(remaining));
    if (ret > 0) {
      metrics->tx_bytes_total.fetch_add(static_cast<uint64_t>(ret), std::memory_order_relaxed);
      item.offset += static_cast<std::size_t>(ret);
      if (item.offset >= item.data.size()) {
        // 当前消息发送完成，从队列头弹出继续发送下一项。
        c->outbound.pop_front();
      }
      continue;
    }

    int err = SSL_get_error(c->ssl, ret);
    if (err == SSL_ERROR_WANT_WRITE) {
      // 暂时不可写，保留当前 offset，等待下一次写事件续发。
      updateInterest(epoll_fd, c->fd, true);
      return false;
    }
    if (err == SSL_ERROR_WANT_READ) {
      // 某些 TLS 状态下写流程会要求先读，切回读事件等待状态推进。
      updateInterest(epoll_fd, c->fd, false);
      return false;
    }

    LOG_ERROR("发送", "连接ID=" + std::to_string(c->id) + " SSL_write致命错误，错误码=" + std::to_string(err));
    return false;
  }

  updateInterest(epoll_fd, c->fd, false);
  return true;
}

static int createListenSocket(uint16_t port) {
  // 函数：创建监听 socket，完成 bind/listen/non-blocking 初始化。
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket() failed");

  int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    throw std::runtime_error("bind() failed");
  }
  if (::listen(fd, SOMAXCONN) < 0) {
    ::close(fd);
    throw std::runtime_error("listen() failed");
  }

  if (setNonBlocking(fd) < 0) {
    ::close(fd);
    throw std::runtime_error("failed to set non-blocking listen socket");
  }
  return fd;
}

// —— 明文 HTTP（--http-port）：与 WSS 不同端口，避免把 HTTP 字节误送入 TLS 监听（浏览器 POST 日志必须打此端口）——
// 支持：GET 静态文件；POST /__wss_browser_log 一行正文；OPTIONS 预检（CORS）；全程非阻塞 send/recv。

// 一条浏览器到 http_port 的 TCP 连接状态机（与 Connection/ssl 无关）。
struct HttpConnection {
  // 累积从 socket 读入的字节，直到出现 HTTP 头结束标记 \r\n\r\n（或 POST 体收集中）。
  std::vector<uint8_t> read_buf;
  // POST /__wss_browser_log 时，头已齐、正文分片到达的累积缓冲。
  std::vector<uint8_t> post_body_accum;
  // POST 期望正文总长度（由 Content-Length 解析）。
  std::size_t post_content_length{0};
  // 待发送的完整 HTTP 响应（状态行+头+体），可能较大故用 char 缓冲区。
  std::vector<char> write_buf;
  // write_buf 已发送到的偏移，用于处理非阻塞 send 的 partial write。
  std::size_t write_off{0};
  enum class Phase {
    ReadingHeaders,    // 收请求头
    ReadingPostBody,   // 仅用于 POST 浏览器日志：收齐 Content-Length 指定字节
    WritingResponse    // 请求已解析，正在发响应
  } phase{Phase::ReadingHeaders};
};

// 更新明文 TCP 连接在 epoll 中的读写关注（与 updateInterest 对应，但不经过 SSL）。
static void setPlainSocketInterest(int epoll_fd, int fd, bool want_in, bool want_out) {
  epoll_event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.data.fd = fd;  // epoll_wait 返回时通过 data.fd 区分是哪条连接。
  ev.events = baseInterest();  // ET + RDHUP，与 WSS 连接一致。
  if (want_in) ev.events |= EPOLLIN;    // 需要继续 recv 请求时置位。
  if (want_out) ev.events |= EPOLLOUT;  // 需要 send 响应时置位。
  epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

// 从 epoll 与连接表中移除一条 HTTP 连接并关闭 fd（不发 TLS shutdown）。
static void closeHttpConnection(int epoll_fd, std::unordered_map<int, HttpConnection>* http_conns, int fd) {
  if (!http_conns->count(fd)) return;  // 防止重复关闭。
  http_conns->erase(fd);
  epoll_event dummy;
  std::memset(&dummy, 0, sizeof(dummy));
  epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &dummy);  // 先从 epoll 摘除，再 close。
  ::close(fd);
  LOG_DEBUG("HTTP静态", "HTTP连接已关闭，fd=" + std::to_string(fd));
}

// 从 read_buf 的首行解析 HTTP 请求行，仅支持 "GET <path> HTTP/x.x" 形式，path 写入 path_out。
static bool parseGetPath(const std::vector<uint8_t>& buf, std::string* path_out) {
  std::size_t line_end = 0;
  // 扫描第一个 \r\n，即请求行结束（不要求完整头部在此函数内校验）。
  for (; line_end + 1 < buf.size(); ++line_end) {
    if (buf[line_end] == '\r' && buf[line_end + 1] == '\n') break;
  }
  if (line_end + 1 >= buf.size()) return false;
  std::string line(reinterpret_cast<const char*>(buf.data()), line_end);
  if (line.size() < 4 || line.compare(0, 4, "GET ") != 0) return false;
  std::size_t p = 4;
  while (p < line.size() && line[p] == ' ') ++p;  // 跳过 GET 后多余空格。
  std::size_t q = line.find(' ', p);              // 路径与 HTTP 版本之间的空格。
  if (q == std::string::npos) return false;
  *path_out = line.substr(p, q - p);  // 例如 "/index.html"。
  return true;
}

// 解析首行 "METHOD <path> HTTP/x.x"（line_end 为请求行末尾 \r 的索引）。
static bool parseHttpRequestLine(const std::vector<uint8_t>& buf, std::size_t line_end, std::string* method,
                                 std::string* path) {
  if (line_end + 1 >= buf.size()) return false;
  std::string line(reinterpret_cast<const char*>(buf.data()), line_end);
  const std::size_t sp1 = line.find(' ');
  if (sp1 == std::string::npos || sp1 == 0) return false;
  *method = line.substr(0, sp1);
  std::size_t p = sp1 + 1;
  while (p < line.size() && line[p] == ' ') ++p;
  const std::size_t sp2 = line.find(' ', p);
  if (sp2 == std::string::npos) return false;
  *path = line.substr(p, sp2 - p);
  return true;
}

static constexpr const char* kBrowserLogHttpPath = "/__wss_browser_log";  // 与 web/index.html 中 fetch 路径一致
static constexpr std::size_t kMaxBrowserLogBody = 4096;  // 单行日志上限，防恶意大包占内存

// 从头块（从首字节到「空行上一行」的末尾，不含最后的 \r\n\r\n）解析 Content-Length。
static bool parseContentLengthHeader(const std::string& header_block, std::size_t* out_len) {
  std::istringstream iss(header_block);
  std::string hline;
  while (std::getline(iss, hline)) {
    if (!hline.empty() && hline.back() == '\r') hline.pop_back();
    if (hline.empty()) continue;
    std::string key;
    std::size_t ci = 0;
    for (; ci < hline.size() && hline[ci] != ':'; ++ci) {
      key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(hline[ci]))));
    }
    if (ci >= hline.size() || hline[ci] != ':') continue;
    if (key != "content-length") continue;
    std::size_t v = ci + 1;
    while (v < hline.size() && (hline[v] == ' ' || hline[v] == '\t')) ++v;
    try {
      *out_len = static_cast<std::size_t>(std::stoull(hline.substr(v)));
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}

// 根据 URL 路径在 http_root 下拼磁盘路径，读取文件并生成 HTTP/1.1 响应到 out。
static void buildHttpStaticResponse(const std::string& http_root, const std::string& url_path,
                                    std::vector<char>* out) {
  // 闭包：统一生成 404 小响应，避免重复字符串。
  auto not_found = [out]() {
    const char* s =
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: "
        "close\r\nContent-Length: 9\r\n\r\nnot found";
    out->assign(s, s + std::strlen(s));
  };
  std::string p = url_path;
  if (p.empty() || p == "/") p = "/index.html";  // 根路径默认演示页（与常见静态站 index 约定一致）。
  if (p.find("..") != std::string::npos) {  // 禁止路径穿越。
    not_found();
    return;
  }
  if (p == "/favicon.ico") {  // 浏览器自动请求，返回无内容减少噪音。
    const char* s = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
    out->assign(s, s + std::strlen(s));
    return;
  }
  if (!p.empty() && p[0] == '/') p = p.substr(1);  // 去掉前导 /，与 http_root 拼接。
  std::string full = http_root;
  if (!full.empty() && full.back() != '/') full += '/';
  full += p;
  std::ifstream ifs(full.c_str(), std::ios::binary);
  if (!ifs) {
    not_found();
    return;
  }
  std::string body((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  std::ostringstream head;
  head << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: "
       << body.size() << "\r\n\r\n";
  std::string h = head.str();
  out->clear();
  out->reserve(h.size() + body.size());
  out->insert(out->end(), h.begin(), h.end());
  out->insert(out->end(), body.begin(), body.end());
}

// 请求过大或首行非法时，切换为发送 400 并进入写阶段。
static void sendBadHttpRequest(int epoll_fd, int fd, HttpConnection* hc) {
  const char* s = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
  hc->write_buf.assign(s, s + std::strlen(s));
  hc->write_off = 0;
  hc->phase = HttpConnection::Phase::WritingResponse;
  setPlainSocketInterest(epoll_fd, fd, false, true);  // 只等可写。
}

static void sendHttp204CorsClose(int epoll_fd, int fd, HttpConnection* hc, const char* extra_headers) {
  const char* base = "HTTP/1.1 204 No Content\r\nConnection: close\r\n";
  std::string s = std::string(base) + (extra_headers ? extra_headers : "") + "\r\n";
  hc->write_buf.assign(s.begin(), s.end());
  hc->write_off = 0;
  hc->phase = HttpConnection::Phase::WritingResponse;
  setPlainSocketInterest(epoll_fd, fd, false, true);
}

// EPOLLIN：先收齐 \r\n\r\n；再按方法分派 OPTIONS / POST 浏览器日志 / GET 静态；POST 体可能分片故有两阶段。
static void driveHttpRead(int epoll_fd, int fd, HttpConnection* hc, const std::string& http_root,
                          std::unordered_map<int, HttpConnection>* http_conns,
                          const std::string& browser_log_dir) {
  char tmp[4096];  // 栈上缓冲，避免每连接堆分配

  if (hc->phase == HttpConnection::Phase::ReadingPostBody) {
    for (;;) {  // 直到收齐 Content-Length 或出错
      ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
      if (n > 0) {
        hc->post_body_accum.insert(hc->post_body_accum.end(), tmp, tmp + static_cast<std::size_t>(n));  // 追加正文分片
        if (hc->post_body_accum.size() > kMaxBrowserLogBody) {  // 超过上限视为攻击或错误配置
          sendBadHttpRequest(epoll_fd, fd, hc);
          return;
        }
        if (hc->post_body_accum.size() >= hc->post_content_length) {  // 已收满一行（或约定的一帧正文）
          std::string line(reinterpret_cast<const char*>(hc->post_body_accum.data()), hc->post_content_length);  // 只取 cl 字节
          if (!browser_log_dir.empty()) {  // 空目录：端点应已在首段拒绝，此处双保险
            if (appendBrowserClientLogFile(browser_log_dir, line)) {
              LOG_INFO("HTTP静态", "浏览器日志 POST 已落盘（分块收齐），body 字节=" + std::to_string(line.size()));
            } else {
              LOG_WARN("HTTP静态", "浏览器日志 POST 落盘失败（检查 log_dir 权限或磁盘）");
            }
          }
          sendHttp204CorsClose(epoll_fd, fd, hc, "Access-Control-Allow-Origin: *\r\n");  // 204 + CORS，浏览器 fetch 不报错
          hc->read_buf.clear();
          hc->post_body_accum.clear();
        }
        if (hc->phase == HttpConnection::Phase::WritingResponse) return;  // sendBadHttpRequest 等已切写阶段
        continue;
      }
      if (n == 0) {  // 对端关连接
        closeHttpConnection(epoll_fd, http_conns, fd);
        return;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // 非阻塞：暂无数据
      closeHttpConnection(epoll_fd, http_conns, fd);  // 真错
      return;
    }
  }

  for (;;) {
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n > 0) {
      hc->read_buf.insert(hc->read_buf.end(), tmp, tmp + static_cast<std::size_t>(n));
      if (hc->read_buf.size() > 16384) {
        sendBadHttpRequest(epoll_fd, fd, hc);
        return;
      }
      for (std::size_t i = 0; i + 3 < hc->read_buf.size(); ++i) {
        if (hc->read_buf[i] != '\r' || hc->read_buf[i + 1] != '\n' || hc->read_buf[i + 2] != '\r' ||
            hc->read_buf[i + 3] != '\n')
          continue;

        const std::size_t header_end = i;
        std::size_t line0_end = 0;
        for (; line0_end + 1 < hc->read_buf.size(); ++line0_end) {
          if (hc->read_buf[line0_end] == '\r' && hc->read_buf[line0_end + 1] == '\n') break;
        }
        if (line0_end + 1 >= hc->read_buf.size() || line0_end > header_end) {
          sendBadHttpRequest(epoll_fd, fd, hc);
          return;
        }
        std::string method;
        std::string path;
        if (!parseHttpRequestLine(hc->read_buf, line0_end, &method, &path)) {
          sendBadHttpRequest(epoll_fd, fd, hc);
          return;
        }
        for (char& ch : method) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        if (path == kBrowserLogHttpPath && method == "options") {
          sendHttp204CorsClose(epoll_fd, fd, hc,
                               "Access-Control-Allow-Origin: *\r\n"
                               "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
                               "Access-Control-Allow-Headers: Content-Type\r\n");
          hc->read_buf.clear();
          return;
        }

        if (path == kBrowserLogHttpPath && method == "post") {  // 浏览器镜像日志唯一入口
          if (browser_log_dir.empty()) {  // main_server 未传 log_dir 或显式关闭
            const char* s =
                "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Type: text/plain; "
                "charset=utf-8\r\nContent-Length: 28\r\n\r\nbrowser log endpoint disabled";
            hc->write_buf.assign(s, s + std::strlen(s));
            hc->write_off = 0;
            hc->phase = HttpConnection::Phase::WritingResponse;
            setPlainSocketInterest(epoll_fd, fd, false, true);
            hc->read_buf.clear();
            return;
          }
          std::string header_text(reinterpret_cast<const char*>(hc->read_buf.data()), header_end);  // 不含空行后 body
          std::size_t cl = 0;
          if (!parseContentLengthHeader(header_text, &cl) || cl == 0 || cl > kMaxBrowserLogBody) {  // 必须带 Content-Length
            sendBadHttpRequest(epoll_fd, fd, hc);
            return;
          }
          const std::size_t body_off = i + 4;  // 跳过 \r\n\r\n 四字节，指向 body 首字节
          const std::size_t have = hc->read_buf.size() - body_off;  // 同一 recv 缓冲区里已有多少 body
          if (have >= cl) {  // 首包已带齐正文（常见：小日志一行）
            std::string body(reinterpret_cast<const char*>(hc->read_buf.data() + body_off), cl);
            if (appendBrowserClientLogFile(browser_log_dir, body)) {
              LOG_INFO("HTTP静态", "浏览器日志 POST 已落盘（首包含 body），body 字节=" + std::to_string(body.size()));
            } else {
              LOG_WARN("HTTP静态", "浏览器日志 POST 落盘失败（检查 log_dir 权限或磁盘）");
            }
            sendHttp204CorsClose(epoll_fd, fd, hc, "Access-Control-Allow-Origin: *\r\n");
            hc->read_buf.clear();
            return;
          }
          hc->post_content_length = cl;  // 记录尚缺字节数
          hc->post_body_accum.assign(hc->read_buf.begin() + static_cast<std::ptrdiff_t>(body_off),
                                     hc->read_buf.end());  // 已到的部分 body
          hc->read_buf.clear();
          hc->phase = HttpConnection::Phase::ReadingPostBody;  // 下轮 EPOLLIN 只走 post_body 分片逻辑
          setPlainSocketInterest(epoll_fd, fd, true, false);
          return;
        }

        if (method != "get") {
          sendBadHttpRequest(epoll_fd, fd, hc);
          return;
        }
        std::string get_path;
        if (!parseGetPath(hc->read_buf, &get_path)) {
          sendBadHttpRequest(epoll_fd, fd, hc);
          return;
        }
        buildHttpStaticResponse(http_root, get_path, &hc->write_buf);
        hc->write_off = 0;
        hc->phase = HttpConnection::Phase::WritingResponse;
        setPlainSocketInterest(epoll_fd, fd, false, true);
        return;
      }
      continue;
    }
    if (n == 0) {
      closeHttpConnection(epoll_fd, http_conns, fd);
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    closeHttpConnection(epoll_fd, http_conns, fd);
    return;
  }
}

// EPOLLOUT 时调用：尽量把 write_buf 发完；发完则关闭连接（HTTP/1.1 亦用 Connection: close 简化）。
static void driveHttpWrite(int epoll_fd, int fd, HttpConnection* hc,
                           std::unordered_map<int, HttpConnection>* http_conns) {
  while (hc->write_off < hc->write_buf.size()) {
    ssize_t n =
        ::send(fd, hc->write_buf.data() + hc->write_off, hc->write_buf.size() - hc->write_off, MSG_NOSIGNAL);
    if (n > 0) {
      hc->write_off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;  // 等下次 EPOLLOUT。
    closeHttpConnection(epoll_fd, http_conns, fd);
    return;
  }
  closeHttpConnection(epoll_fd, http_conns, fd);  // 全部发出，短连接结束。
}

}  // namespace

WssServer::WssServer() = default;
WssServer::~WssServer() = default;

// port/cert/key：TLS+WSS 监听与证书；httpPort/httpRoot：可选明文静态 HTTP（与前者独立端口）。
bool WssServer::run(uint16_t port, const std::string& certFile, const std::string& keyFile, uint16_t httpPort,
                    const std::string& httpRoot, int wsIdleTimeoutSec, int wsServerPingIntervalSec,
                    const std::string& browserClientLogDir) {
  // 函数：服务端主入口，负责从初始化到事件循环的完整生命周期。
  try {
    Logger::setMinLevel(LogLevel::Info);
    const std::string kBrowserLogDir = browserClientLogDir;
    const int idleTimeoutSec = (wsIdleTimeoutSec <= 0) ? 120 : wsIdleTimeoutSec;
    int pingIntervalSec = wsServerPingIntervalSec;
    if (pingIntervalSec <= 0) {
      pingIntervalSec = std::max(5, idleTimeoutSec / 3);
    }
    if (pingIntervalSec >= idleTimeoutSec) {
      pingIntervalSec = std::max(5, idleTimeoutSec / 2);
    }
    LOG_INFO("服务器", "服务启动中，目标端口=" + std::to_string(port) + "，WS空闲超时=" +
                           std::to_string(idleTimeoutSec) + "秒，服务端Ping间隔=" + std::to_string(pingIntervalSec) +
                           "秒");
    // OpenSSL global init.
    OPENSSL_init_ssl(0, nullptr);

    SSL_CTX* ctx = openssl_helpers::createServerContext(certFile, keyFile);
    if (!ctx) throw std::runtime_error("createServerContext returned null");

    int listen_fd = createListenSocket(port);
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) throw std::runtime_error("epoll_create1 failed");

    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.data.fd = listen_fd;
    ev.events = EPOLLIN | baseInterest();
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
      // 监听 fd 注册失败时直接抛错，避免进入不一致状态。
      throw std::runtime_error("epoll_ctl ADD listen_fd failed");
    }

    std::unordered_map<int, std::shared_ptr<Connection>> conns;  // fd -> TLS+WebSocket 连接。
    std::unordered_map<int, HttpConnection> http_conns;          // fd -> 明文 HTTP 客户端连接。
    int http_listen_fd{-1};  // 明文 HTTP 监听 fd；-1 表示未启用（httpPort==0）。
    uint64_t nextConnId = 1;

    std::vector<epoll_event> events(1024);
    LOG_INFO("服务器", "监听成功，端口=" + std::to_string(port) + "，协议=TLS1.3");

    std::size_t workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0) workerCount = 8;
    workerCount = std::max<std::size_t>(2, std::min<std::size_t>(16, workerCount));
    ThreadPool pool(workerCount);
    FileTransferManager fileManager;
    ServerMetrics metrics;

    // TLS 读路径热缓冲：固定 16KB 与当前 SSL_read 长度一致，预分配 512 块供多连接复用；
    // 耗尽时 MemoryPool 内部回退堆分配并打 WARN（见 MemoryPool.cpp）。
    constexpr std::size_t kTlsReadBytes = 16 * 1024;
    constexpr std::size_t kTlsReadPoolBlocks = 512;
    MemoryPool tlsReadPool(kTlsReadBytes, kTlsReadPoolBlocks, "TlsRead");

    // Heartbeat timer: scan connections periodically（空闲踢线 + 服务端 Ping）。
    const int scanSeconds = 1;
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) throw std::runtime_error("timerfd_create failed");

    itimerspec its;
    std::memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = scanSeconds;
    its.it_interval.tv_sec = scanSeconds;
    if (timerfd_settime(timer_fd, 0, &its, nullptr) < 0) {
      // 定时器配置失败时及时释放 fd，防止资源泄露。
      ::close(timer_fd);
      throw std::runtime_error("timerfd_settime failed");
    }

    epoll_event tev;
    std::memset(&tev, 0, sizeof(tev));
    tev.data.fd = timer_fd;
    tev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &tev) < 0) {
      ::close(timer_fd);
      throw std::runtime_error("epoll_ctl ADD timer_fd failed");
    }

    // 线程池入队 outbound 后唤醒 IO 线程执行 SSL_write（避免仅 epoll_ctl EPOLLOUT 在 ET+跨线程下迟迟不唤醒）。
    int wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd < 0) throw std::runtime_error("eventfd failed");
    epoll_event wev;
    std::memset(&wev, 0, sizeof(wev));
    wev.data.fd = wake_fd;
    wev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &wev) < 0) {
      ::close(wake_fd);
      throw std::runtime_error("epoll_ctl ADD wake_fd failed");
    }

    // 明文 HTTP：与 WSS 共用 epoll，仅提供静态页（如 web/index.html）；与 TLS 监听 port 不冲突。
    if (httpPort != 0) {
      http_listen_fd = createListenSocket(httpPort);  // 复用与 TLS 相同的非阻塞 listen 封装。
      epoll_event hev;
      std::memset(&hev, 0, sizeof(hev));
      hev.data.fd = http_listen_fd;  // 事件分发时与 listen_fd、timer_fd 区分。
      hev.events = EPOLLIN | baseInterest();
      if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, http_listen_fd, &hev) < 0) {
        ::close(http_listen_fd);
        http_listen_fd = -1;
        throw std::runtime_error("epoll_ctl ADD http_listen_fd failed");
      }
      LOG_INFO("HTTP静态", "内置明文HTTP已注册到epoll，端口=" + std::to_string(httpPort) + "，根目录=" + httpRoot);
    }

    while (true) {
      int n = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), 1000);
      if (n < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("epoll_wait failed");
      }

      for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        uint32_t e = events[i].events;

        if (fd == timer_fd) {
          // Drain timerfd.
          while (true) {
            uint64_t expirations = 0;
            ssize_t r = ::read(timer_fd, &expirations, sizeof(expirations));
            if (r < 0) {
              if (errno == EAGAIN || errno == EWOULDBLOCK) break;
              // timerfd 读取失败属于系统级异常，交由外层 catch 处理。
              throw std::runtime_error("timerfd read failed");
            }
            if (r == 0) break;
          }

          auto now = std::chrono::steady_clock::now();
          const auto pingIv = std::chrono::seconds(pingIntervalSec);

          // 1) 对已升级的连接按间隔发送服务端 Ping（浏览器会自动回 Pong），并刷新 last_ping 避免竞态。
          for (auto& kv : conns) {
            const std::shared_ptr<Connection>& c = kv.second;
            if (!c->tls_done || !c->ws_upgraded || c->closing) continue;
            auto sincePing =
                std::chrono::duration_cast<std::chrono::seconds>(now - c->last_server_ping_sent);
            if (sincePing < pingIv) continue;

            std::vector<uint8_t> empty_pl;
            std::vector<uint8_t> pingFrame = WebSocketCodec::buildPing(empty_pl);
            {
              std::lock_guard<std::mutex> lk(c->outbound_mu);
              OutboundItem item;
              item.data = std::move(pingFrame);
              item.offset = 0;
              c->outbound.push_back(std::move(item));
              updatePeak(&metrics.tx_queue_peak, static_cast<uint64_t>(c->outbound.size()));
            }
            c->last_server_ping_sent = now;
            c->last_ping = now;
            updateInterest(epoll_fd, kv.first, true);
            LOG_DEBUG("心跳", "连接ID=" + std::to_string(c->id) + " 发送服务端Ping");
          }
          // 定时器在 IO 线程：入队 Ping 后主动冲刷，避免与业务线程 outbound 相同地卡在 epoll 边沿。
          for (auto& kv : conns) {
            auto cc = kv.second;
            if (!cc->tls_done || cc->closing) continue;
            (void)flushOutbound(epoll_fd, cc, &metrics);
          }

          // 2) 空闲超时：超过 idleTimeoutSec 未刷新 last_ping 则关闭（无数据且未成功保活）。
          std::vector<int> toClose;
          toClose.reserve(conns.size());
          for (const auto& kv : conns) {
            const auto& c = kv.second;
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - c->last_ping);
            if (diff.count() > idleTimeoutSec) {
              toClose.push_back(kv.first);
            }
          }
          for (int cfd : toClose) {
            LOG_INFO("服务器", "连接空闲超时关闭，fd=" + std::to_string(cfd) + "，阈值=" +
                                   std::to_string(idleTimeoutSec) + "秒");
            closeConnection(epoll_fd, conns, cfd);
          }

          // 同步触发文件会话清理：
          // - 空闲超过 10 分钟清理；
          // - 会话上限 1024，超过按最近更新时间淘汰最旧会话。
          {
            uint64_t now_seconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            fileManager.cleanupSessions(now_seconds, 600, 1024);
          }

          // 每5秒输出一次观测指标快照。
          if ((std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() % 5) == 0) {
            ThreadPool::Stats ts = pool.stats();
            LOG_INFO("指标",
                     "线程池(submitted=" + std::to_string(ts.submitted) +
                         ",completed=" + std::to_string(ts.completed) +
                         ",queue_cur=" + std::to_string(ts.queue_current) +
                         ",queue_peak=" + std::to_string(ts.queue_peak) +
                         "),发送队列峰值=" + std::to_string(metrics.tx_queue_peak.load()) +
                         ",累计发送字节=" + std::to_string(metrics.tx_bytes_total.load()) +
                         ",握手成功=" + std::to_string(metrics.handshake_ok.load()) +
                         ",握手失败=" + std::to_string(metrics.handshake_fail.load()) +
                         ",复用握手=" + std::to_string(metrics.handshake_reused.load()) +
                         ",新握手=" + std::to_string(metrics.handshake_new.load()));
          }
          continue;
        }

        if (fd == wake_fd) {
          // 工作线程在入队 outbound 后 write(eventfd)；此处排空计数并在 IO 线程冲刷 SSL_write。
          while (true) {
            uint64_t cnt = 0;
            ssize_t r = ::read(wake_fd, &cnt, sizeof(cnt));
            if (r < 0) {
              if (errno == EAGAIN || errno == EWOULDBLOCK) break;
              throw std::runtime_error("wake_fd read failed");
            }
            if (r == 0) break;
          }
          LOG_DEBUG("服务器",
                    "eventfd 已唤醒 IO 线程，尝试冲刷 outbound（连接数=" + std::to_string(conns.size()) + "）");
          for (auto& kv : conns) {
            auto cc = kv.second;
            if (!cc->tls_done || cc->closing) continue;
            (void)flushOutbound(epoll_fd, cc, &metrics);
          }
          continue;
        }

        // —— 明文 HTTP：监听套接字上的可读＝有新连接接入（ET 下 accept 直到 EAGAIN）。——
        if (http_listen_fd >= 0 && fd == http_listen_fd) {
          while (true) {
            sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int cfd = ::accept(http_listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (cfd < 0) {
              if (errno == EAGAIN || errno == EWOULDBLOCK) break;
              throw std::runtime_error("http accept() failed");
            }
            if (setNonBlocking(cfd) < 0) {
              ::close(cfd);
              continue;
            }
            http_conns.emplace(cfd, HttpConnection{});  // 初始为 ReadingHeaders，只收请求。
            epoll_event cev;
            std::memset(&cev, 0, sizeof(cev));
            cev.data.fd = cfd;
            cev.events = EPOLLIN | baseInterest();
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
              http_conns.erase(cfd);
              ::close(cfd);
              continue;
            }
            LOG_DEBUG("HTTP静态", "新HTTP连接，fd=" + std::to_string(cfd));
          }
          continue;
        }

        // —— 明文 HTTP：已连接套接字上的读/写（必须在 TLS conns 分派之前命中）。——
        {
          auto hit = http_conns.find(fd);
          if (hit != http_conns.end()) {
            HttpConnection* hc = &hit->second;
            if (e & EPOLLERR) {
              LOG_WARN("HTTP静态", "套接字错误(EPOLLERR)，fd=" + std::to_string(fd));
              closeHttpConnection(epoll_fd, &http_conns, fd);
              continue;
            }
            // 无读无写仅挂断：对端已关闭，直接清理（与 WSS 侧语义类似）。
            if ((e & (EPOLLHUP | EPOLLRDHUP)) && !(e & EPOLLIN) && !(e & EPOLLOUT)) {
              closeHttpConnection(epoll_fd, &http_conns, fd);
              continue;
            }
            // 先处理写：若本轮同时可读可写，优先把响应推出去（driveHttpWrite 可能关闭 fd）。
            if (hc->phase == HttpConnection::Phase::WritingResponse && (e & EPOLLOUT)) {
              driveHttpWrite(epoll_fd, fd, hc, &http_conns);
            }
            // 写路径可能已 erase，需重新 find 再读请求。
            auto it2 = http_conns.find(fd);
            if (it2 != http_conns.end() &&
                (it2->second.phase == HttpConnection::Phase::ReadingHeaders ||
                 it2->second.phase == HttpConnection::Phase::ReadingPostBody) &&
                (e & EPOLLIN)) {
              driveHttpRead(epoll_fd, fd, &it2->second, httpRoot, &http_conns, kBrowserLogDir);
            }
            continue;  // 已消费该 fd，勿落入 TLS 分支。
          }
        }

        if (fd == listen_fd) {
          // Accept until EAGAIN.
          while (true) {
            sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (cfd < 0) {
              if (errno == EAGAIN || errno == EWOULDBLOCK) break;
              // 在 ET 模式下，非 EAGAIN 异常应视为真实 accept 失败。
              throw std::runtime_error("accept() failed");
            }
            if (setNonBlocking(cfd) < 0) {
              ::close(cfd);
              continue;
            }

            SSL* ssl = SSL_new(ctx);
            if (!ssl) {
              ::close(cfd);
              continue;
            }
            SSL_set_accept_state(ssl);
            SSL_set_fd(ssl, cfd);

            auto c = std::make_shared<Connection>(cfd, ssl, nextConnId++);
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
            // ctx 未启用 early（max_early_data==0）时，无 SSL_read_early_data 阶段，等价于「early 已 FINISH」。
            c->tls_early_read_finished = (SSL_CTX_get_max_early_data(ctx) == 0);  // 非 0 则必须先 driveTlsEarlyRead
#else
            c->tls_early_read_finished = true;  // 无编译期 early API，走纯 SSL_accept
#endif
            conns[cfd] = c;
            LOG_INFO("连接", "新连接接入，fd=" + std::to_string(cfd) + "，连接ID=" + std::to_string(c->id));

            epoll_event cev;
            std::memset(&cev, 0, sizeof(cev));
            cev.data.fd = cfd;
            cev.events = EPOLLIN | baseInterest();
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
              // 子连接注册失败，立即执行统一关闭，避免半初始化连接残留。
              closeConnection(epoll_fd, conns, cfd);
              continue;
            }
          }
          continue;
        }

        auto it = conns.find(fd);
        if (it == conns.end()) continue;
        auto c = it->second;

        // epoll 挂断/错误语义区分（避免把客户端正常退出当成“异常”）：
        // - EPOLLERR：套接字错误，需告警；
        // - EPOLLHUP / EPOLLRDHUP：对端关闭写端或连接结束，常见于客户端 SSL_shutdown + close 后的 FIN；
        //   若同时带有 EPOLLIN，优先走下方 SSL_read，由 ret==0 分支统一记“对端主动关闭”。
        if (e & EPOLLERR) {
          LOG_WARN("连接", "套接字错误(EPOLLERR)，fd=" + std::to_string(fd) + "，连接ID=" +
                              std::to_string(c->id) + "，准备关闭");
          closeConnection(epoll_fd, conns, fd);
          continue;
        }
        if ((e & (EPOLLHUP | EPOLLRDHUP)) && !(e & EPOLLIN)) {
          // 无待读数据时的挂断：直接回收，使用信息级别（非异常）。
          LOG_INFO("连接", "对端已关闭连接（挂断/半关闭，无待读数据），fd=" + std::to_string(fd) +
                               "，连接ID=" + std::to_string(c->id));
          closeConnection(epoll_fd, conns, fd);
          continue;
        }

        if ((e & EPOLLIN) || ((e & EPOLLOUT) && !c->tls_done)) {
          // 握手阶段：可读或「可写且未完成握手」都会进来（SSL_accept 可能 WANT_WRITE）。
          if (!c->tls_done) {
            int adv = advanceTlsHandshake(epoll_fd, conns, c, &metrics, &tlsReadPool, pool, fileManager, wake_fd);
            if (adv < 0) {  // early 或 accept 硬失败
              closeConnection(epoll_fd, conns, fd);
              continue;
            }
            if (adv == 0) {  // WANT_READ/WRITE，保持连接
              if (e & EPOLLOUT) {
                (void)flushOutbound(epoll_fd, c, &metrics);  // 少数情况下 accept 前后队列已有数据（通常空）
              }
              continue;
            }
            // adv==1：本轮回调内握手刚完成，tls_done=true；101/Pong 可能已在 outbound 排队，立即 flush 避免 ET 下卡在无 EPOLLOUT。
            (void)flushOutbound(epoll_fd, c, &metrics);
          }
        }

        if (e & EPOLLIN) {
          if (c->tls_done) {
            // SSL_read loop：每轮迭代从内存池借 16KB 缓冲，离开作用域自动归还（TlsReadBufferGuard）。
            while (true) {
              TlsReadBufferGuard bufGuard(&tlsReadPool);
              uint8_t* buf = bufGuard.data();
              const int bufLen = static_cast<int>(bufGuard.size());
              int ret = SSL_read(c->ssl, buf, bufLen);
              if (ret > 0) {
                bool stop_burst = false;
                if (!processWsInboundBuffer(epoll_fd, conns, fd, c, &metrics, pool, fileManager, wake_fd, buf, ret,
                                            &stop_burst)) {
                  break;
                }
                if (stop_burst || c->closing) break;
                continue;
              }
              if (ret == 0) {
                LOG_INFO("连接", "对端主动关闭连接，fd=" + std::to_string(fd));
                closeConnection(epoll_fd, conns, fd);
                break;
              }

              int err = SSL_get_error(c->ssl, ret);
              if (err == SSL_ERROR_WANT_READ) break;
              if (err == SSL_ERROR_WANT_WRITE) {
                // 当前读取流程因 TLS 状态要求可写，动态切换关注事件。
                updateInterest(epoll_fd, fd, true);
                break;
              }

              closeConnection(epoll_fd, conns, fd);
              break;
            }
          }
        }

        if (e & EPOLLOUT) {
          // EPOLLOUT 只做“发送队列冲刷”，不做任何业务处理。
          bool drained = flushOutbound(epoll_fd, c, &metrics);
          if (drained && c->closing) {
            closeConnection(epoll_fd, conns, fd);
          }
        }
      }
    }

    // Unreachable.
    SSL_CTX_free(ctx);
    ::close(listen_fd);
    ::close(epoll_fd);
    return true;
  } catch (const std::exception& ex) {
    LOG_ERROR("服务器", std::string("服务运行失败：") + ex.what());
    return false;
  }
}

