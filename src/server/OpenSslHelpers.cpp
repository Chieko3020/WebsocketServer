#include "server/OpenSslHelpers.h"

/*
 * 文件作用说明：
 * 该文件实现 TLS 上下文构建与错误封装逻辑。
 * 目标：
 * 1) 降低上层业务对 OpenSSL 细节的耦合；
 * 2) 在错误发生时返回可读异常信息；
 * 3) 强制项目使用 TLS1.3，契合课题安全目标。
 */

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>

#include "common/Logger.h"

#include <cstdlib>
#include <stdexcept>

namespace openssl_helpers {

static void throwOnOpenSslError(const char* what) {
  // 从 OpenSSL 错误栈取最近错误，并转成可读字符串。
  unsigned long err = ERR_get_error();
  char buf[256];
  ERR_error_string_n(err, buf, sizeof(buf));
  throw std::runtime_error(std::string(what) + ": " + buf);
}

SSL_CTX* createServerContext(const std::string& certFile, const std::string& keyFile) {
  LOG_INFO("TLS", "开始创建服务端SSL上下文");
  // 创建服务端 SSL_CTX，底层使用 TLS_server_method。
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) {
    throwOnOpenSslError("SSL_CTX_new failed");
  }

  // 强制只允许 TLS1.3，避免协商降级到旧版本。
  // 注意：这里限定最小/最大版本都为 TLS1.3，保证行为可预期。
  SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

  // 加载服务端证书。
  if (SSL_CTX_use_certificate_file(ctx, certFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
    // 证书加载失败直接抛错，避免服务在“无证书”状态启动。
    throwOnOpenSslError("load certificate failed");
  }
  // 加载服务端私钥。
  if (SSL_CTX_use_PrivateKey_file(ctx, keyFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
    // 私钥加载失败直接抛错，避免后续握手阶段才暴露问题。
    throwOnOpenSslError("load private key failed");
  }
  // 检查证书与私钥是否匹配。
  if (!SSL_CTX_check_private_key(ctx)) {
    throw std::runtime_error("private key check failed");
  }

  // 禁用旧式重协商，减少兼容性与安全面问题。
  SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
  // 会话缓存：服务端开启缓存，便于后续复用握手。
  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);

  // 参数化：会话超时（秒），默认 300。
  long timeout_seconds = 300;
  if (const char* t = std::getenv("WSS_SESSION_TIMEOUT")) {
    try {
      long parsed = std::stol(t);
      // 仅接收正值，非法值回退默认值。
      if (parsed > 0) timeout_seconds = parsed;
    } catch (...) {
      // 环境变量解析失败时保持默认配置，不中断服务启动。
    }
  }
  SSL_CTX_set_timeout(ctx, timeout_seconds);

  // 参数化：是否启用 session ticket（默认启用）。
  bool enable_ticket = true;
  if (const char* v = std::getenv("WSS_ENABLE_TICKET")) {
    // 约定：字符串 "0" 表示关闭，其他值按开启处理。
    if (std::string(v) == "0") enable_ticket = false;
  }
  if (!enable_ticket) {
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
  }

  // —— 以下为 TLS 1.3 0-RTT（early application data）服务端开关；由 main_server 根据 --enable-0rtt / enable_0rtt 写入环境变量 ——
  bool enable_0rtt = false;  // 默认关闭：与浏览器演示兼容（浏览器无法带 X-Nonce，见 WebSocketCodec）
  if (const char* v = std::getenv("WSS_ENABLE_0RTT")) {  // 读取子进程环境（非配置文件直传，便于与 OpenSSL 初始化同文件）
    if (std::string(v) == "1") enable_0rtt = true;  // 仅当值为字符串 "1" 时视为开启
  }
  if (enable_0rtt) {
    // 非零 max_early_data 告诉 OpenSSL：本 ctx 上可能出现 0-RTT 应用数据，服务端必须先走 SSL_read_early_data
    // 直至 SSL_READ_EARLY_DATA_FINISH，再 SSL_accept；否则违反 API 契约（见 WssServer::driveTlsEarlyRead）。
    SSL_CTX_set_max_early_data(ctx, 16 * 1024);  // 字节上限：与客户端 CLI 一致，覆盖 WS 升级 HTTP + X-Nonce
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    // OpenSSL 默认 anti-replay：首次 0-RTT 后可能使 ticket「单次有效」，同 PEM 二次连或难稳定 ACCEPTED。
    // 教学演示需要反复用同一 session 文件验证路径，故显式关闭 anti-replay（缩小生产可用性，仅实验）。
    SSL_CTX_set_options(ctx, SSL_OP_NO_ANTI_REPLAY);  // 位或进已有 options（上面已含 SSL_OP_NO_RENEGOTIATION）
    LOG_WARN("TLS",
             "0-RTT 实验：已启用 SSL_OP_NO_ANTI_REPLAY，允许 ticket/会话 PEM 多次复用以严格验证 early data（削弱 TLS1.3 内置重放防护）");
#endif
  } else {
    SSL_CTX_set_max_early_data(ctx, 0);  // 零表示「无 early data」：连接建立后直接 SSL_accept，无需 early 读阶段
  }

  LOG_INFO("TLS", "服务端SSL上下文创建成功（TLS1.3）");
  LOG_INFO("TLS", std::string("会话参数：ticket=") + (enable_ticket ? "开启" : "关闭") +
                      "，session_timeout=" + std::to_string(timeout_seconds) + "秒");
  LOG_INFO("TLS", std::string("0-RTT实验开关：") + (enable_0rtt ? "开启" : "关闭"));
  return ctx;
}

SSL* createClientSSL(SSL_CTX* ctx) {
  // 该函数用于创建客户端方向 SSL 会话对象。
  SSL* ssl = SSL_new(ctx);
  if (!ssl) throw std::runtime_error("SSL_new failed");
  // 设置为“客户端握手角色”，后续由 SSL_connect 推进状态机。
  SSL_set_connect_state(ssl);
  return ssl;
}

}  // namespace openssl_helpers

