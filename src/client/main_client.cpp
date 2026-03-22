#include "ws/WebSocketCodec.h"

/*
 * 文件作用说明：
 * 该文件实现命令行客户端，用于端到端验证服务器功能。
 * 客户端能力包括：
 * 1) 建立 TLS1.3 连接并校验服务端证书；
 * 2) 完成 WebSocket Upgrade 握手；
 * 3) 发送文本消息并验证回显；
 * 4) 按分片协议上传文件并支持断点续传；
 * 5) 周期发送 Ping，避免心跳超时被服务端关闭。
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "common/Crc32.h"
#include "common/ErrorCodes.h"
#include "common/Logger.h"

namespace {

// 函数：打印客户端命令行参数说明。
static void usage(const char* prog) {
  std::cerr << "用法: " << prog
            << " --host <ip> --port <port> --ca <ca.pem> "
               "[--text <msg>] [--file <path>] [--chunk-size <bytes>] "
               "[--enable-ticket <0|1>] [--session-timeout <sec>] [--enable-0rtt <0|1>] "
               "[--session-file <path>] [--log-dir <dir>] [--no-log-file] "
               "[--client-ping-interval <sec>]\n"
            << "  --session-file：TLS 会话 PEM 路径；与 --enable-0rtt 1 配合用于第二次连接 0-RTT early data（默认 $HOME/.cache/wss_client_session.pem）。\n"
            << "  --log-dir：除 stderr 外追加写入 <dir>/wss_client.log（默认 log）。\n"
            << "  --no-log-file：禁用客户端文件日志。\n"
            << "  --client-ping-interval：CLI 周期性发送 WebSocket Ping 的间隔秒数，默认 10；0=不发送（依赖服务端 Ping）。\n";
}

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
// 从磁盘 PEM 恢复上次 TLS 1.3 会话（含 ticket / max_early_data 等），供 SSL_set_session 使用。
static SSL_SESSION* loadSessionPem(const std::string& path) {
  if (path.empty()) return nullptr;  // 未配置路径：首次连接无会话
  BIO* bio = BIO_new_file(path.c_str(), "rb");  // 只读打开
  if (!bio) return nullptr;  // 文件不存在或不可读
  SSL_SESSION* sess = PEM_read_bio_SSL_SESSION(bio, nullptr, nullptr, nullptr);  // OpenSSL 解析 PEM
  BIO_free(bio);  // 释放 BIO，不释放 sess（调用方负责 SSL_SESSION_free）
  return sess;  // 可能为 nullptr（格式错）
}

// 握手结束后把 SSL_SESSION 写回 PEM，使第二次运行可 load + set_session + write_early_data。
static bool saveSessionPem(SSL* ssl, const std::string& path) {
  if (path.empty()) return false;
  SSL_SESSION* sess = SSL_get1_session(ssl);  // 引用计数 +1，需配对 free
  if (!sess) return false;
  BIO* bio = BIO_new_file(path.c_str(), "wb");  // 覆盖写
  if (!bio) {
    SSL_SESSION_free(sess);
    return false;
  }
  int ok = PEM_write_bio_SSL_SESSION(bio, sess);  // 序列化会话
  BIO_free(bio);
  SSL_SESSION_free(sess);
  return ok == 1;
}

// 与 WssServer 侧 earlyDataStatusLabel 对齐，便于对照实验日志。
static const char* earlyDataStatusLabelClient(const SSL* ssl) {
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

// 在「未走 early 写」路径上：读空 TLS 层 post-handshake 记录，使 NewSessionTicket 合并进 SSL_SESSION，再 save PEM。
// 若已 early 写了 HTTP，则 101 已进 TLS 层，不能再乱读，否则吞掉 WS 响应（见下方 !wroteEarlyData 判断）。
static void tlsAbsorbPostHandshakeForSession(SSL* ssl) {
  unsigned char buf[16 * 1024];
  for (int round = 0; round < 32; ++round) {  // 有限轮次，避免死循环
    int n = SSL_read(ssl, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)));
    if (n > 0) {
      LOG_WARN("TLS", std::string("握手后读吸收 unexpected 应用数据 ") + std::to_string(n) +
                         " 字节，停止吸收（请检查协议顺序）");
      return;  // 不应读到应用数据（WebSocket 尚未开始）
    }
    if (n == 0) {
      int e = SSL_get_error(ssl, n);
      if (e == SSL_ERROR_ZERO_RETURN) return;  // 对端关
    }
    int err = SSL_get_error(ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return;  // 无更多记录可吸
    return;
  }
}

// 将整段 HTTP 升级请求作为 TLS 1.3 early application data 发送，再 SSL_connect 完成握手（与 OpenSSL 推荐顺序一致）。
static void sendEarlyDataThenFinishHandshake(SSL* ssl, const std::string& req) {
  std::size_t off = 0;  // 已写入 early 的明文偏移
  while (off < req.size()) {
    size_t written = 0;  // 本次 API 实际消费的明文字节数
    int w = SSL_write_early_data(ssl, req.data() + off, req.size() - off, &written);
    if (w == 1) {  // 成功写入一段（可能仍剩）
      off += written;
      continue;
    }
    int err = SSL_get_error(ssl, w);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      int cr = SSL_connect(ssl);  // 推进握手，使后续可继续 early 写
      if (cr == 1) {
        continue;
      }
      int e2 = SSL_get_error(ssl, cr);
      if (e2 == SSL_ERROR_WANT_READ || e2 == SSL_ERROR_WANT_WRITE) continue;
      throw std::runtime_error("SSL_connect during early write failed");
    }
    throw std::runtime_error("SSL_write_early_data failed");
  }
  while (true) {  // early 已全部提交，继续 connect 直到握手完成
    int cr = SSL_connect(ssl);
    if (cr == 1) return;
    int e2 = SSL_get_error(ssl, cr);
    if (e2 == SSL_ERROR_WANT_READ || e2 == SSL_ERROR_WANT_WRITE) continue;
    throw std::runtime_error("SSL_connect failed");
  }
}
#endif

// 函数：持续读取直到收到 HTTP 头结束标记（\r\n\r\n）。
static bool readUntilDoubleCrlf(SSL* ssl, std::string* out) {
  out->clear();
  std::vector<char> buf(4096);
  while (true) {
    int n = SSL_read(ssl, buf.data(), static_cast<int>(buf.size()));
    if (n > 0) {
      out->append(buf.data(), buf.data() + n);
      auto pos = out->find("\r\n\r\n");
      if (pos != std::string::npos) return true;
    } else {
      int err = SSL_get_error(ssl, n);
      // 非阻塞状态机可重试错误：继续读直到收齐头部。
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
      return false;
    }
  }
}

// 函数：确保 TLS 写入完整字节序列（处理 partial write）。
static void sslWriteAll(SSL* ssl, const uint8_t* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    int n = SSL_write(ssl, data + off, static_cast<int>(len - off));
    if (n > 0) {
      // 成功写入 n 字节，推进发送偏移直至写完整包。
      off += static_cast<std::size_t>(n);
      continue;
    }
    int err = SSL_get_error(ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
    throw std::runtime_error("SSL_write failed");
  }
}

// 函数重载：vector 版本写入，内部调用指针版本。
static void sslWriteAll(SSL* ssl, const std::vector<uint8_t>& bytes) {
  sslWriteAll(ssl, bytes.data(), bytes.size());
}

// 函数：字符串转字节数组（当前项目里保留作工具函数）。
static std::vector<uint8_t> toBytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

// 函数：Base64 编码工具。
static std::string base64Encode(const unsigned char* input, int len) {
  std::string out;
  out.resize(static_cast<std::size_t>(4 * ((len + 2) / 3)));
  int outLen = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), input, len);
  out.resize(static_cast<std::size_t>(outLen));
  return out;
}

// 函数：从升级响应中提取 Sec-WebSocket-Accept。
static std::string getWsAcceptFromResponse(const std::string& resp) {
  // Server response is small; do a simple extraction.
  const std::string key = "Sec-WebSocket-Accept:";
  auto pos = resp.find(key);
  if (pos == std::string::npos) return {};
  pos += key.size();
  while (pos < resp.size() && (resp[pos] == ' ' || resp[pos] == '\t')) pos++;
  auto end = resp.find("\r\n", pos);
  if (end == std::string::npos) return {};
  return resp.substr(pos, end - pos);
}

// 函数：FNV-1a 64 位哈希，用于生成稳定 file_id。
static uint64_t fnv1a64(const std::string& s) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : s) {
    hash ^= static_cast<uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

// 函数：读取网络字节序 u32。
static uint32_t readU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// 函数：读取网络字节序 u64。
static uint64_t readU64BE(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint64_t>(p[i]);
  return v;
}

// 函数：写入网络字节序 u16。
static void writeU16BE(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(v & 0xFFu));
}

// 函数：写入网络字节序 u32。
static void writeU32BE(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(v & 0xFFu));
}

// 函数：写入网络字节序 u64。
static void writeU64BE(std::vector<uint8_t>* out, uint64_t v) {
  for (int i = 7; i >= 0; --i) out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

// 函数：从路径中取出文件名（用于 FILE_START 附带展示名）。
static std::string fileBasename(const std::string& path) {
  std::size_t pos = path.find_last_of("/\\");
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// 函数：计算本地文件 CRC32。
static uint32_t computeFileCrc32(const std::string& path) {
  std::ifstream ifs(path.c_str(), std::ios::binary);
  if (!ifs) throw std::runtime_error("open file failed for crc32");
  CRC32 crc;
  crc.reset();
  std::vector<uint8_t> buf(64 * 1024);
  while (ifs) {
    ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    std::streamsize n = ifs.gcount();
    if (n > 0) crc.update(buf.data(), static_cast<std::size_t>(n));
  }
  return crc.finish();
}

// 函数：获取本地文件大小（字节）。
static uint64_t getFileSize(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) < 0) throw std::runtime_error("stat failed");
  return static_cast<uint64_t>(st.st_size);
}

// 函数：判断 bitmap 中某分片是否缺失（缺失返回 true）。
static bool bitmapMissing(const std::vector<uint8_t>& bitmap, uint32_t idx) {
  const uint32_t byte = idx / 8;
  const uint32_t bit = idx % 8;
  if (byte >= bitmap.size()) return true;
  bool have = ((bitmap[byte] >> bit) & 0x1u) != 0;
  return !have;
}

constexpr uint8_t MSG_FILE_START = 1;
constexpr uint8_t MSG_FILE_QUERY = 2;
constexpr uint8_t MSG_FILE_CHUNK = 3;

constexpr uint8_t MSG_FILE_QUERY_RESPONSE = 101;
constexpr uint8_t MSG_FILE_CHUNK_ACK = 102;
constexpr uint8_t MSG_FILE_FINISH_ACK = 103;
constexpr uint8_t MSG_FILE_ERROR = 255;

}  // namespace

int main(int argc, char** argv) {
  // 客户端主流程：
  // 1) 参数解析
  // 2) TCP+TLS 建链
  // 3) WebSocket 升级
  // 4) 文本回显验证
  // 5) 文件分片上传与续传
  // 6) 关闭连接
  Logger::setMinLevel(LogLevel::Info);
  std::string host = "127.0.0.1";
  uint16_t port = 0;
  std::string caFile;
  std::string text = "hello";
  std::string filePath;
  uint32_t chunkSize = 64 * 1024;
  int enableTicket = 1;
  long sessionTimeout = 300;
  // 0-RTT 实验：默认关闭，与服务器 enable_0rtt 默认一致；与浏览器演示兼容。测 0-RTT 时传 --enable-0rtt 1。
  int enable0Rtt = 0;
  // 会话 PEM 路径（第二次连接复用 + early data）；空表示未指定，将在 enable-0rtt 时填默认路径。
  std::string sessionFile;
  // 浏览器无法主动发 Ping；CLI 可周期性发客户端 Ping 保活，默认 10 秒；0 表示关闭（仅用服务端 Ping）。
  int clientPingIntervalSec = 10;
  // 与 wss_server 的 log_dir 对齐：CLI 追加写入 log/wss_client.log。
  std::string logDir = "log";
  bool enableClientFileLog = true;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    // 参数解析按“键 + 值”模式处理，遇到未知参数直接报用法并退出。
    if (arg == "--host" && i + 1 < argc) host = argv[++i];
    else if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
    else if (arg == "--ca" && i + 1 < argc) caFile = argv[++i];
    else if (arg == "--text" && i + 1 < argc) text = argv[++i];
    else if (arg == "--file" && i + 1 < argc) filePath = argv[++i];
    else if (arg == "--chunk-size" && i + 1 < argc) chunkSize = static_cast<uint32_t>(std::stoul(argv[++i]));
    else if (arg == "--enable-ticket" && i + 1 < argc) enableTicket = std::atoi(argv[++i]);
    else if (arg == "--session-timeout" && i + 1 < argc) sessionTimeout = std::atol(argv[++i]);
    else if (arg == "--enable-0rtt" && i + 1 < argc) enable0Rtt = std::atoi(argv[++i]);
    else if (arg == "--session-file" && i + 1 < argc) sessionFile = argv[++i];
    else if (arg == "--log-dir" && i + 1 < argc) logDir = argv[++i];
    else if (arg == "--no-log-file") enableClientFileLog = false;
    else if (arg == "--client-ping-interval" && i + 1 < argc)
      clientPingIntervalSec = std::atoi(argv[++i]);
    else {
      usage(argv[0]);
      return 2;
    }
  }

  if (port == 0 || caFile.empty()) {
    usage(argv[0]);
    return 2;
  }
  if (sessionFile.empty() && enable0Rtt != 0) {
    const char* home = std::getenv("HOME");
    if (home) sessionFile = std::string(home) + "/.cache/wss_client_session.pem";
  }

  if (enableClientFileLog) {
    if (!Logger::initFileLogWithName(logDir, "wss_client.log")) {
      std::cerr << "[警告] wss_client 无法打开文件日志（目录: " << logDir << "），仅 stderr。\n";
    } else {
      std::atexit(+[]() { Logger::shutdownFileLog(); });
      LOG_INFO("客户端", "文件日志已启用（与 stderr 同步）：" + logDir + "/wss_client.log");
    }
  }

  try {
  LOG_INFO("客户端", "启动客户端，目标地址=" + host + ":" + std::to_string(port));
  LOG_INFO("客户端",
            std::string("参数摘要：0-RTT实验=") + (enable0Rtt != 0 ? "开启(升级含X-Nonce)" : "关闭") +
                "，会话文件=" + (sessionFile.empty() ? "(无)" : sessionFile) +
                "，文件日志=" + (enableClientFileLog ? (logDir + "/wss_client.log") : "关闭") +
                "，客户端Ping间隔=" + (clientPingIntervalSec > 0 ? std::to_string(clientPingIntervalSec) + "秒" : "关闭"));

  SSL_library_init();
  OPENSSL_init_ssl(0, nullptr);

  // Resolve + connect.
  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  std::string portStr = std::to_string(port);
  if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
    throw std::runtime_error("getaddrinfo failed");
  }

  int sock = -1;
  for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
    // 逐候选地址尝试连接，兼容 IPv4/IPv6 返回结果。
    sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sock < 0) continue;
    if (::connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
    ::close(sock);
    sock = -1;
  }
  freeaddrinfo(res);
  if (sock < 0) throw std::runtime_error("connect failed");
  LOG_INFO("客户端", "TCP连接建立成功");

  // Read timeout so we can send periodic pings.
  timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // —— TLS 客户端上下文 ——
  // 仅 TLS 1.3，与服务器 OpenSslHelpers 行为对齐；后续 SSL_connect / SSL_write_early_data 均依赖此 ctx。
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) throw std::runtime_error("SSL_CTX_new failed");
  SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
  // 客户端会话缓存：用于首次完整握手拿到 NewSessionTicket，第二次 SSL_set_session 恢复。
  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
  SSL_CTX_set_timeout(ctx, sessionTimeout);
  if (enableTicket == 0) {
    // 对照实验：无 ticket 则通常无法 0-RTT（无 resumption 信息）。
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
  }
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  if (SSL_CTX_load_verify_locations(ctx, caFile.c_str(), nullptr) != 1) {
    throw std::runtime_error("load CA failed");
  }

  // 客户端 ctx 的 max_early_data 与服务端上限对齐；真正能否发送 early 还看 session 里 PEM 携带的配额。
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
  if (enable0Rtt != 0) {
    SSL_CTX_set_max_early_data(ctx, 16 * 1024);  // 与 OpenSslHelpers 服务端 16KiB 一致
  } else {
    SSL_CTX_set_max_early_data(ctx, 0);  // 关闭 early：仅普通握手 + 握手后 SSL_write(req)
  }
#endif

  // 明文 HTTP（RFC6455 升级）字节串；在 TLS 保护下作为 application data 发送（常规在握手后，0-RTT 在握手前）。
  unsigned char randKey[16];
  if (RAND_bytes(randKey, sizeof(randKey)) != 1) throw std::runtime_error("RAND_bytes failed");
  std::string wsKey = base64Encode(randKey, static_cast<int>(sizeof(randKey)));  // Sec-WebSocket-Key
  std::string req = "GET / HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "Upgrade: websocket\r\n";
  req += "Connection: Upgrade\r\n";
  req += "Sec-WebSocket-Key: " + wsKey + "\r\n";
  req += "Sec-WebSocket-Version: 13\r\n\r\n";
  if (enable0Rtt != 0) {  // 与 WebSocketCodec::shouldEnforceNonce() 配套：服务端要求带 X-Nonce
    unsigned char nonceRaw[16];
    if (RAND_bytes(nonceRaw, sizeof(nonceRaw)) != 1) throw std::runtime_error("RAND_bytes nonce failed");
    std::string nonce = base64Encode(nonceRaw, static_cast<int>(sizeof(nonceRaw)));
    req = "GET / HTTP/1.1\r\n"
          "Host: " + host + "\r\n" +
          "Upgrade: websocket\r\n" +
          "Connection: Upgrade\r\n" +
          "Sec-WebSocket-Key: " + wsKey + "\r\n" +
          "Sec-WebSocket-Version: 13\r\n" +
          "X-Nonce: " + nonce + "\r\n\r\n";  // 额外头：服务端用于最小重放检测（浏览器无法发送）
  }

  SSL_SESSION* sessFromFile = nullptr;  // 上次保存的会话（可能为 nullptr）
  uint32_t maxEarlyFromSession = 0;  // PEM 中记录的 max_early_data，决定能否 write_early_data
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
  if (enable0Rtt != 0 && !sessionFile.empty()) {  // 首次连接无 PEM：走普通握手，save 后再二次连
    sessFromFile = loadSessionPem(sessionFile);
    if (sessFromFile) {
      maxEarlyFromSession = SSL_SESSION_get_max_early_data(sessFromFile);  // 常为 0 直到吸收 NST
    } else {
      LOG_INFO("TLS", "会话 PEM 未加载（文件不存在或格式错误）：" + sessionFile);
    }
  }
#else
  (void)sessFromFile;
  (void)maxEarlyFromSession;
#endif

  SSL* ssl = SSL_new(ctx);
  if (!ssl) throw std::runtime_error("SSL_new failed");
  SSL_set_connect_state(ssl);  // 客户端角色
  SSL_set_fd(ssl, sock);

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
  if (sessFromFile) {
    SSL_set_session(ssl, sessFromFile);  // 恢复会话，使第二次连接可尝试 0-RTT
    SSL_SESSION_free(sessFromFile);  // SSL 内部已复制，释放加载的指针
    sessFromFile = nullptr;
  }

  bool wroteEarlyData = false;  // 若 true，则 req 已随 early 发出，禁止再 sslWriteAll(req)
  const bool tryEarlyWrite =
      (enable0Rtt != 0 && maxEarlyFromSession > 0 && req.size() <= maxEarlyFromSession);  // 配额与长度检查
  LOG_INFO("TLS", std::string("0-RTT 诊断：PEM.max_early_data=") + std::to_string(maxEarlyFromSession) +
                       " upgrade_bytes=" + std::to_string(req.size()) +
                       " try_SSL_write_early_data=" + (tryEarlyWrite ? "1" : "0"));
  if (tryEarlyWrite) {
    sendEarlyDataThenFinishHandshake(ssl, req);  // 先 early 写满 HTTP，再 connect 完成握手
    wroteEarlyData = true;
  } else {
    while (true) {  // 常规：完整握手
      int ret = SSL_connect(ssl);
      if (ret == 1) break;
      int err = SSL_get_error(ssl, ret);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
      throw std::runtime_error("SSL_connect failed");
    }
  }
#else
  bool wroteEarlyData = false;
  while (true) {
    int ret = SSL_connect(ssl);
    if (ret == 1) break;
    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
    throw std::runtime_error("SSL_connect failed");
  }
#endif

  LOG_INFO("TLS", "TLS1.3 握手成功");
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
  // 首次连接：握手后读入 TLS 层 NST，更新内存中的 SSL_SESSION，使 max_early_data>0，再 save PEM。
  // 已 early 发送：101 已在 TLS 缓冲，禁止此处 SSL_read，否则 readUntilDoubleCrlf 永远等不到。
  if (enable0Rtt != 0 && !sessionFile.empty() && !wroteEarlyData) {
    timeval tvFast;
    tvFast.tv_sec = 0;
    tvFast.tv_usec = 200000;  // 200ms：避免阻塞过久
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tvFast, sizeof(tvFast));
    tlsAbsorbPostHandshakeForSession(ssl);
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));  // 恢复主循环 1s 读超时
    const SSL_SESSION* s = SSL_get0_session(ssl);
    if (s) {
      LOG_INFO("TLS", std::string("会话吸收后 max_early_data=") + std::to_string(SSL_SESSION_get_max_early_data(s)));
    }
  }
  LOG_INFO("TLS", std::string("TLS_session_reused=") + (SSL_session_reused(ssl) ? "1" : "0"));
  LOG_INFO("TLS", std::string("early_data_status=") + earlyDataStatusLabelClient(ssl) +
                       (wroteEarlyData ? " (HTTP 升级已通过 early data 发送)" : ""));
  if (!sessionFile.empty()) {
    if (saveSessionPem(ssl, sessionFile)) {
      LOG_INFO("TLS", "已保存 TLS 会话 PEM：" + sessionFile);
    } else {
      LOG_INFO("TLS", "未能写入会话文件（请检查目录是否存在及权限）");
    }
  }
#endif
  LOG_INFO("TLS", std::string("客户端会话参数：ticket=") + (enableTicket == 0 ? "关闭" : "开启") +
                      "，session_timeout=" + std::to_string(sessionTimeout) + "秒");

  if (!wroteEarlyData) {
    sslWriteAll(ssl, reinterpret_cast<const uint8_t*>(req.data()), req.size());  // 握手后补发 HTTP（非 early）
  }

  std::string resp;
  if (!readUntilDoubleCrlf(ssl, &resp)) throw std::runtime_error("read websocket upgrade response failed");
  std::string accept = getWsAcceptFromResponse(resp);
  if (accept.empty()) throw std::runtime_error("missing Sec-WebSocket-Accept");
  std::string expectedAccept = WebSocketCodec::computeAccept(wsKey);
  if (accept != expectedAccept) throw std::runtime_error("Sec-WebSocket-Accept mismatch");

  LOG_INFO("WebSocket", "WebSocket 握手成功");
  if (clientPingIntervalSec > 0) {
    LOG_INFO("客户端", "已启用 CLI 客户端 Ping，间隔=" + std::to_string(clientPingIntervalSec) + "秒（DEBUG 级别可查看每次发送）");
  } else {
    LOG_INFO("客户端", "未启用 CLI 客户端 Ping，仅依赖服务端 Ping 保活");
  }

  // Parser for server->client frames (unmasked).
  WebSocketStreamParser wsParser(false);
  wsParser.setOpenMode();
  std::string dummyAccept;

  auto sendPingIfDue = [&](std::chrono::steady_clock::time_point& lastPingSent) {
    if (clientPingIntervalSec <= 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPingSent < std::chrono::seconds(clientPingIntervalSec)) return;
    std::vector<uint8_t> pingPayload;
    auto pingFrame = WebSocketCodec::buildClientPing(pingPayload);
    sslWriteAll(ssl, pingFrame);
    lastPingSent = now;
    LOG_DEBUG("客户端", "已发送 WebSocket Ping（间隔=" + std::to_string(clientPingIntervalSec) + "秒）");
  };

  auto readFramesOnce = [&](std::vector<WsFrame>* outFrames,
                            std::chrono::steady_clock::time_point& lastPingSent) -> bool {
    uint8_t buf[8192];
    int n = SSL_read(ssl, buf, sizeof(buf));
    if (n > 0) {
      outFrames->clear();
      std::vector<WsFrame> frames;
      try {
        (void)wsParser.feed(buf, static_cast<std::size_t>(n), &dummyAccept, &frames);
      } catch (...) {
        throw;
      }
      *outFrames = std::move(frames);
      return !outFrames->empty();
    }

    int err = SSL_get_error(ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      // 当前暂无可消费应用数据：借机维持心跳，避免服务端超时踢线。
      sendPingIfDue(lastPingSent);
      return false;
    }
    // Timeout from SO_RCVTIMEO can manifest as WANT_READ; treat as not fatal.
    sendPingIfDue(lastPingSent);
    return false;
  };

  // Send text and wait for echo.
  sslWriteAll(ssl, WebSocketCodec::buildClientTextFrame(text));

  std::chrono::steady_clock::time_point lastPingSent = std::chrono::steady_clock::now();
  bool gotText = false;
  while (!gotText) {
    sendPingIfDue(lastPingSent);
    std::vector<WsFrame> frames;
    try {
      readFramesOnce(&frames, lastPingSent);
    } catch (const std::exception& ex) {
      throw;
    }
    for (const auto& f : frames) {
      if (f.opcode == 0x1) {
        std::string got(f.payload.begin(), f.payload.end());
        LOG_INFO("业务", "收到文本回显，内容=" + got);
        gotText = true;
      }
    }
  }

  if (filePath.empty()) {
    LOG_INFO("客户端", "未指定文件，文本测试结束后退出");
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(sock);
    return 0;
  }

  const uint64_t fileSize = getFileSize(filePath);
  const uint32_t totalChunks = static_cast<uint32_t>((fileSize + chunkSize - 1) / chunkSize);
  const uint32_t expectedCrc32 = computeFileCrc32(filePath);
  const uint64_t fileId = fnv1a64(filePath);

  // FILE_START
  std::vector<uint8_t> startPayload;
  startPayload.push_back(MSG_FILE_START);
  writeU64BE(&startPayload, fileId);
  // file_size
  for (int i = 7; i >= 0; --i) startPayload.push_back(static_cast<uint8_t>((fileSize >> (8 * i)) & 0xFFu));
  writeU32BE(&startPayload, chunkSize);
  writeU32BE(&startPayload, totalChunks);
  writeU32BE(&startPayload, expectedCrc32);
  {
    std::string base = fileBasename(filePath);
    constexpr std::size_t kMaxFn = 512;
    if (base.size() > kMaxFn) base.resize(kMaxFn);
    const uint16_t fnLen = static_cast<uint16_t>(base.size());
    writeU16BE(&startPayload, fnLen);
    for (char c : base) {
      startPayload.push_back(static_cast<uint8_t>(static_cast<unsigned char>(c)));
    }
  }
  sslWriteAll(ssl, WebSocketCodec::buildClientBinaryFrame(startPayload));

  // FILE_QUERY to get current bitmap.
  std::vector<uint8_t> queryPayload;
  queryPayload.push_back(MSG_FILE_QUERY);
  writeU64BE(&queryPayload, fileId);
  sslWriteAll(ssl, WebSocketCodec::buildClientBinaryFrame(queryPayload));

  std::vector<uint8_t> bitmap;
  uint32_t queryTotalChunks = 0;
  bool gotBitmap = false;
  while (!gotBitmap) {
    sendPingIfDue(lastPingSent);
    std::vector<WsFrame> frames;
    readFramesOnce(&frames, lastPingSent);
    for (const auto& f : frames) {
      if (f.opcode != 0x2) continue;
      if (f.payload.empty()) continue;
      if (f.payload[0] == MSG_FILE_ERROR && f.payload.size() >= 3) {
        uint16_t code = error_codes::readU16BE(f.payload.data() + 1);
        std::string detail;
        if (f.payload.size() > 3) {
          detail.assign(reinterpret_cast<const char*>(f.payload.data() + 3), f.payload.size() - 3);
        }
        LOG_ERROR("文件上传", "服务端错误 code=" + std::to_string(code) + " detail=" + detail);
        throw std::runtime_error("server file protocol error");
      }
      if (f.payload[0] == MSG_FILE_QUERY_RESPONSE) {
        // layout: type(1) + file_id(8) + total_chunks(4) + bitmap_len(4) + bitmap
        if (f.payload.size() < 1 + 8 + 4 + 4) continue;
        const uint8_t* q = f.payload.data() + 1;
        // uint64_t gotFileId = readU64BE(q);
        queryTotalChunks = readU32BE(q + 8);
        uint32_t bitmapLen = readU32BE(q + 12);
        if (f.payload.size() < 1 + 8 + 4 + 4 + bitmapLen) continue;
        bitmap.assign(f.payload.begin() + 1 + 8 + 4 + 4, f.payload.begin() + 1 + 8 + 4 + 4 + bitmapLen);
        gotBitmap = true;
        LOG_INFO("文件上传", "收到FILE_QUERY响应，bitmap字节数=" + std::to_string(bitmap.size()));
      }
    }
  }

  LOG_INFO("文件上传", "缺失分片扫描完成，总分片数=" + std::to_string(totalChunks));

  std::fstream ifs(filePath.c_str(), std::ios::in | std::ios::binary);
  if (!ifs) throw std::runtime_error("open file failed");

  std::vector<uint8_t> chunkBuf;
  chunkBuf.resize(chunkSize);

  for (uint32_t chunkIndex = 0; chunkIndex < totalChunks; ++chunkIndex) {
    // bitmap 标记已存在的分片直接跳过，体现断点续传收益。
    if (!bitmapMissing(bitmap, chunkIndex)) continue;

    const uint64_t offset = static_cast<uint64_t>(chunkIndex) * static_cast<uint64_t>(chunkSize);
    const uint64_t remaining = fileSize - offset;
    const uint32_t curLen = static_cast<uint32_t>(std::min<uint64_t>(remaining, chunkSize));

    ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    ifs.read(reinterpret_cast<char*>(chunkBuf.data()), static_cast<std::streamsize>(curLen));
    std::size_t gotLen = static_cast<std::size_t>(ifs.gcount());
    if (gotLen != curLen) throw std::runtime_error("read chunk failed");

    CRC32 crc;
    uint32_t chunkCrc = crc.checksum(chunkBuf.data(), gotLen);

    std::vector<uint8_t> chunkPayload;
    chunkPayload.push_back(MSG_FILE_CHUNK);
    writeU64BE(&chunkPayload, fileId);
    writeU32BE(&chunkPayload, chunkIndex);
    writeU32BE(&chunkPayload, chunkCrc);
    chunkPayload.insert(chunkPayload.end(), chunkBuf.begin(), chunkBuf.begin() + gotLen);

    sslWriteAll(ssl, WebSocketCodec::buildClientBinaryFrame(chunkPayload));

    // Wait for chunk ack.
    bool gotAck = false;
    while (!gotAck) {
      sendPingIfDue(lastPingSent);
      std::vector<WsFrame> frames;
      readFramesOnce(&frames, lastPingSent);
      for (const auto& f : frames) {
        if (f.opcode != 0x2 || f.payload.empty()) continue;
        if (f.payload[0] == MSG_FILE_ERROR && f.payload.size() >= 3) {
          uint16_t code = error_codes::readU16BE(f.payload.data() + 1);
          std::string detail;
          if (f.payload.size() > 3) {
            detail.assign(reinterpret_cast<const char*>(f.payload.data() + 3), f.payload.size() - 3);
          }
          LOG_ERROR("文件上传", "分片确认阶段收到错误 code=" + std::to_string(code) + " detail=" + detail);
          throw std::runtime_error("server file protocol error");
        }
        if (f.payload[0] != MSG_FILE_CHUNK_ACK) continue;
        if (f.payload.size() < 1 + 8 + 4 + 1) continue;
        const uint8_t* a = f.payload.data() + 1;
        uint64_t ackFileId = readU64BE(a);
        uint32_t ackChunkIndex = readU32BE(a + 8);
        (void)ackFileId;
        if (ackChunkIndex == chunkIndex) {
          gotAck = true;
          break;
        }
      }
    }

    LOG_INFO("文件上传", "分片上传完成，chunk=" + std::to_string(chunkIndex));
  }

  // Wait for finish ack.
  bool gotFinish = false;
  while (!gotFinish) {
    sendPingIfDue(lastPingSent);
    std::vector<WsFrame> frames;
    readFramesOnce(&frames, lastPingSent);
    for (const auto& f : frames) {
      if (f.opcode != 0x2 || f.payload.empty()) continue;
      if (f.payload[0] == MSG_FILE_ERROR && f.payload.size() >= 3) {
        uint16_t code = error_codes::readU16BE(f.payload.data() + 1);
        std::string detail;
        if (f.payload.size() > 3) {
          detail.assign(reinterpret_cast<const char*>(f.payload.data() + 3), f.payload.size() - 3);
        }
        LOG_ERROR("文件上传", "完成阶段收到错误 code=" + std::to_string(code) + " detail=" + detail);
        throw std::runtime_error("server file protocol error");
      }
      if (f.payload[0] == MSG_FILE_FINISH_ACK) {
        // layout: type(1) + file_id(8) + ok(1)
        gotFinish = true;
        LOG_INFO("文件上传", "收到文件完成确认（FILE_FINISH_ACK）");
        break;
      }
    }
  }

  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  ::close(sock);
  LOG_INFO("客户端", "客户端流程执行完毕，连接已关闭");
  return 0;
  } catch (const std::exception& ex) {
    LOG_ERROR("客户端", std::string("异常退出：") + ex.what());
    return 1;
  }
}

