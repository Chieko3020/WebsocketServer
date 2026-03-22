#include "ws/WebSocketCodec.h"
#include "common/Logger.h"

/*
 * 文件作用说明：
 * 该文件实现 WebSocket 协议关键逻辑：
 * 1) 握手阶段：Sec-WebSocket-Accept 计算；
 * 2) 编码阶段：服务端/客户端帧构建；
 * 3) 解码阶段：增量流式解析、分片拼包、控制帧校验。
 *
 * 该模块是服务端与客户端通信协议一致性的核心。
 */

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {
static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 工具函数：将二进制摘要做 Base64 编码，供握手 Accept 计算使用。
std::string base64Encode(const unsigned char* input, int len) {
  std::string out;
  out.resize(static_cast<std::size_t>(4 * ((len + 2) / 3)));
  int outLen = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), input, len);
  out.resize(static_cast<std::size_t>(outLen));
  return out;
}

// 工具函数：把字符串转小写，便于做 Header 名大小写无关匹配。
std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// 工具函数：在 HTTP 头块中查找指定字段值（字段名大小写不敏感）。
bool findHeaderValue(const std::string& headerBlock, const std::string& headerName, std::string* out) {
  // Very small and permissive parser: split by lines and find "Header-Name:" prefix.
  std::istringstream iss(headerBlock);
  std::string line;
  std::string target = toLower(headerName);
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto pos = line.find(':');
    if (pos == std::string::npos) continue;
    std::string name = toLower(line.substr(0, pos));
    if (name == target) {
      std::string value = line.substr(pos + 1);
      while (!value.empty() && value[0] == ' ') value.erase(value.begin());
      *out = value;
      return true;
    }
  }
  return false;
}
// 工具函数：读取环境变量控制的 WS 最大 payload 限制值。
std::size_t maxPayloadLimit() {
  // 允许通过环境变量调整负载上限，便于压测和鲁棒性实验。
  static std::size_t cached = 0;
  if (cached != 0) return cached;
  cached = 64 * 1024;
  const char* v = std::getenv("WSS_MAX_PAYLOAD_BYTES");
  if (!v) return cached;
  try {
    std::size_t parsed = static_cast<std::size_t>(std::stoul(v));
    if (parsed >= 1024) cached = parsed;
  } catch (...) {
    // 保持默认值
  }
  return cached;
}

// 工具函数：判断是否启用 0-RTT 实验模式下的 nonce 校验。
bool shouldEnforceNonce() {
  const char* v = std::getenv("WSS_ENABLE_0RTT");
  return v && std::string(v) == "1";
}

// 工具函数：nonce 去重校验（最小重放防护策略）。
bool acceptNonce(const std::string& nonce) {
  static std::unordered_map<std::string, uint64_t> seen;
  uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  const uint64_t ttl = 300;
  for (auto it = seen.begin(); it != seen.end();) {
    if (now - it->second > ttl) it = seen.erase(it);
    else ++it;
  }
  if (nonce.empty()) return false;
  if (seen.count(nonce)) return false;
  seen[nonce] = now;
  return true;
}
}  // namespace

std::string WebSocketCodec::computeAccept(const std::string& secWebSocketKey) {
  // RFC6455 要求：accept = Base64(SHA1(key + GUID))
  std::string concat = secWebSocketKey + kGuid;
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(concat.data()), concat.size(), hash);
  return base64Encode(hash, SHA_DIGEST_LENGTH);
}

std::vector<uint8_t> WebSocketCodec::buildFrame(uint8_t opcode, const std::vector<uint8_t>& payload, bool fin) {
  std::vector<uint8_t> out;
  out.reserve(2 + payload.size() + 8);

  uint8_t b0 = static_cast<uint8_t>((fin ? 0x80 : 0x00) | (opcode & 0x0F));
  out.push_back(b0);

  // 服务端到客户端：不加 mask。
  uint64_t len = payload.size();
  if (len <= 125) {
    out.push_back(static_cast<uint8_t>(len & 0x7F));
  } else if (len <= 0xFFFFu) {
    out.push_back(126);
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(len & 0xFFu));
  } else {
    out.push_back(127);
    for (int i = 7; i >= 0; --i) {
      out.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFFu));
    }
  }

  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<uint8_t> WebSocketCodec::buildTextFrame(const std::string& text) {
  // 文本帧封装是 buildFrame 的语义化封装。
  return buildFrame(0x1, std::vector<uint8_t>(text.begin(), text.end()), true);
}

std::vector<uint8_t> WebSocketCodec::buildBinaryFrame(const std::vector<uint8_t>& data) {
  // 二进制帧封装是 buildFrame 的语义化封装。
  return buildFrame(0x2, data, true);
}

std::vector<uint8_t> WebSocketCodec::buildPing(const std::vector<uint8_t>& payload) {
  // Ping 帧用于心跳保活。
  return buildFrame(0x9, payload, true);
}

std::vector<uint8_t> WebSocketCodec::buildPong(const std::vector<uint8_t>& payload) {
  // Pong 帧用于响应对端 Ping。
  return buildFrame(0xA, payload, true);
}

std::vector<uint8_t> WebSocketCodec::buildClose(uint16_t statusCode) {
  // Close 帧 payload 前两字节为状态码（网络字节序）。
  std::vector<uint8_t> payload;
  payload.push_back(static_cast<uint8_t>((statusCode >> 8) & 0xFFu));
  payload.push_back(static_cast<uint8_t>(statusCode & 0xFFu));
  return buildFrame(0x8, payload, true);
}

std::vector<uint8_t> WebSocketCodec::buildClientFrame(uint8_t opcode, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out;
  out.reserve(2 + payload.size() + 16);

  // 客户端到服务端：必须加 mask（RFC 强制约束）。
  uint8_t b0 = static_cast<uint8_t>(0x80 | (opcode & 0x0F));
  out.push_back(b0);

  uint64_t len = payload.size();
  if (len <= 125) {
    out.push_back(static_cast<uint8_t>(0x80 | (len & 0x7Fu)));
  } else if (len <= 0xFFFFu) {
    out.push_back(static_cast<uint8_t>(0x80 | 126));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(len & 0xFFu));
  } else {
    out.push_back(static_cast<uint8_t>(0x80 | 127));
    for (int i = 7; i >= 0; --i) {
      out.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFFu));
    }
  }

  uint8_t mask_key[4];
  if (RAND_bytes(mask_key, 4) != 1) {
    // Fallback: deterministic mask if RNG fails (should not happen).
    mask_key[0] = 0x12;
    mask_key[1] = 0x34;
    mask_key[2] = 0x56;
    mask_key[3] = 0x78;
  }

  for (int i = 0; i < 4; ++i) out.push_back(mask_key[i]);

  for (std::size_t i = 0; i < payload.size(); ++i) {
    out.push_back(static_cast<uint8_t>(payload[i] ^ mask_key[i % 4]));
  }

  return out;
}

std::vector<uint8_t> WebSocketCodec::buildClientTextFrame(const std::string& text) {
  // 客户端文本帧：调用通用客户端帧构造器。
  return buildClientFrame(0x1, std::vector<uint8_t>(text.begin(), text.end()));
}

std::vector<uint8_t> WebSocketCodec::buildClientBinaryFrame(const std::vector<uint8_t>& data) {
  // 客户端二进制帧：调用通用客户端帧构造器。
  return buildClientFrame(0x2, data);
}

std::vector<uint8_t> WebSocketCodec::buildClientPing(const std::vector<uint8_t>& payload) {
  // 客户端心跳帧。
  return buildClientFrame(0x9, payload);
}

// 默认构造：用于服务端场景，默认要求来向帧必须带掩码。
WebSocketStreamParser::WebSocketStreamParser() {}

// 可配置构造：用于客户端场景可关闭“必须掩码”检查。
WebSocketStreamParser::WebSocketStreamParser(bool requireMaskFromPeer) {
  require_mask_ = requireMaskFromPeer;
}

void WebSocketStreamParser::setOpenMode() {
  // 切换到已握手状态并清空解析上下文。
  state_ = State::Open;
  parse_offset_ = 0;
  buffer_.clear();
  in_fragment_ = false;
  fragment_opcode_ = 0;
  fragment_payload_.clear();
}

bool WebSocketStreamParser::tryConsumeUpgrade(std::string* outAcceptResponse) {
  // 该函数只负责消费 HTTP Upgrade 请求头并构造 101 响应。
  if (!outAcceptResponse) return false;

  static const std::string kCRLFCRLF = "\r\n\r\n";
  if (buffer_.size() < kCRLFCRLF.size()) return false;

  // Find "\r\n\r\n" in buffer_.
  std::size_t endPos = std::string::npos;
  for (std::size_t i = 0; i + kCRLFCRLF.size() <= buffer_.size(); ++i) {
    if (buffer_[i] == '\r' && buffer_[i + 1] == '\n' && buffer_[i + 2] == '\r' && buffer_[i + 3] == '\n') {
      endPos = i + kCRLFCRLF.size();
      break;
    }
  }
  if (endPos == std::string::npos) return false;

  std::string headerBlock(reinterpret_cast<const char*>(buffer_.data()),
                           reinterpret_cast<const char*>(buffer_.data() + endPos));

  std::string secKey;
  if (!findHeaderValue(headerBlock, "Sec-WebSocket-Key", &secKey)) {
    LOG_WARN("WebSocket", "Upgrade请求缺少 Sec-WebSocket-Key");
    throw std::runtime_error("Missing Sec-WebSocket-Key");
  }
  if (secKey.empty()) throw std::runtime_error("Empty Sec-WebSocket-Key");

  // Minimal validation (Upgrade/Connection) to avoid false positives.
  std::string upgrade;
  if (!findHeaderValue(headerBlock, "Upgrade", &upgrade) || toLower(upgrade) != "websocket") {
    LOG_WARN("WebSocket", "Upgrade请求头非法：Upgrade字段不正确");
    throw std::runtime_error("Invalid Upgrade header");
  }

  // —— 与服务端 enable_0rtt 联动：仅实验模式下强制 X-Nonce（浏览器 WebSocket API 无法设自定义头，故演示须关 0-RTT）——
  if (shouldEnforceNonce()) {  // 内部读环境变量/配置，与 OpenSslHelpers 的 WSS_ENABLE_0RTT 同源策略
    std::string nonce;  // Base64 随机串，来自 wss_client --enable-0rtt 1
    if (!findHeaderValue(headerBlock, "X-Nonce", &nonce)) {  // HTTP 头解析失败
      LOG_WARN("WebSocket", "0-RTT实验模式下缺少 X-Nonce");
      throw std::runtime_error("Missing X-Nonce in 0-RTT mode");  // 上层关闭连接，浏览器表现为 1006 等
    }
    if (!acceptNonce(nonce)) {  // 进程内去重：同 nonce 二次出现视为重放
      LOG_WARN("WebSocket", "检测到重复或非法 X-Nonce，拒绝请求");
      throw std::runtime_error("Replay detected by X-Nonce");
    }
  }

  std::string accept = WebSocketCodec::computeAccept(secKey);
  *outAcceptResponse =
      std::string("HTTP/1.1 101 Switching Protocols\r\n") +
      "Upgrade: websocket\r\n" +
      "Connection: Upgrade\r\n" +
      "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

  // Consume header bytes from buffer.
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(endPos));
  parse_offset_ = 0;
  state_ = State::Open;
  LOG_INFO("WebSocket", "HTTP升级请求解析完成，连接进入WS_OPEN状态");
  return true;
}

bool WebSocketStreamParser::feed(const uint8_t* data, std::size_t len, std::string* outAcceptResponse,
                                  std::vector<WsFrame>* outFrames) {
  // 该函数是增量解析主入口：同时处理 Upgrade 和 WS 帧解析。
  if (!outAcceptResponse) return false;
  if (outFrames) outFrames->clear();

  outAcceptResponse->clear();
  if (len > 0) buffer_.insert(buffer_.end(), data, data + len);

  bool producedAccept = false;

  if (state_ == State::AwaitingHttpUpgrade) {
    try {
      producedAccept = tryConsumeUpgrade(outAcceptResponse);
    } catch (...) {
      // Bubble up to caller; WssServer will close connection on errors.
      throw;
    }
    // After consuming upgrade headers, the buffer_ may still contain WS frame bytes.
    if (state_ == State::AwaitingHttpUpgrade) {
      // Not enough bytes yet to finish HTTP upgrade parsing.
      return producedAccept;
    }
  }

  const std::size_t kMaxPayload = maxPayloadLimit();

  // 尽可能多地解析完整帧（处理粘包和半包）。
  while (true) {
    if (buffer_.size() - parse_offset_ < 2) break;
    std::size_t idx = parse_offset_;

    uint8_t b0 = buffer_[idx];
    uint8_t b1 = buffer_[idx + 1];

    bool fin = (b0 & 0x80) != 0;
    uint8_t opcode = b0 & 0x0F;
    bool rsv = (b0 & 0x70) != 0;
    if (rsv) {
      LOG_WARN("WebSocket", "收到非法帧：RSV位被置位");
      throw std::runtime_error("RSV bits set");
    }

    bool masked = (b1 & 0x80) != 0;
    uint64_t payload_len = static_cast<uint64_t>(b1 & 0x7F);
    idx += 2;

    if (opcode >= 0x8 && opcode <= 0xF) {
      if (!fin) throw std::runtime_error("Control frames must not be fragmented");
      if (payload_len > 125) {
        LOG_WARN("WebSocket", "控制帧负载长度非法，payload_len=" + std::to_string(payload_len));
        throw std::runtime_error("Control frame payload too large");
      }
    }

    if (payload_len == 126) {
      if (buffer_.size() - idx < 2) break;
      payload_len = (static_cast<uint64_t>(buffer_[idx]) << 8) | static_cast<uint64_t>(buffer_[idx + 1]);
      idx += 2;
    } else if (payload_len == 127) {
      if (buffer_.size() - idx < 8) break;
      payload_len = 0;
      for (int i = 0; i < 8; ++i) {
        payload_len = (payload_len << 8) | static_cast<uint64_t>(buffer_[idx + i]);
      }
      idx += 8;
    }

    if (payload_len > kMaxPayload) {
      LOG_WARN("WebSocket", "收到超大负载帧，长度=" + std::to_string(payload_len));
      throw std::runtime_error("WS payload too large");
    }

    uint8_t mask_key[4] = {0, 0, 0, 0};
    if (masked) {
      if (buffer_.size() - idx < 4) break;
      std::memcpy(mask_key, &buffer_[idx], 4);
      idx += 4;
    } else {
      if (require_mask_) {
        LOG_WARN("WebSocket", "收到未掩码帧，但当前解析器要求必须掩码");
        throw std::runtime_error("Masked bit not set on incoming frame");
      }
    }

    if (buffer_.size() - idx < payload_len) break;

    std::vector<uint8_t> payload;
    payload.resize(static_cast<std::size_t>(payload_len));
    if (payload_len > 0) {
      std::memcpy(payload.data(), &buffer_[idx], payload_len);
    }

    // Unmask in-place.
    if (masked) {
      for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = payload[i] ^ mask_key[i % 4];
      }
    }
    idx += static_cast<std::size_t>(payload_len);

    // Consume.
    parse_offset_ = idx;

    // 分片消息处理：将 continuation 合并为完整消息后再对上层可见。
    if (opcode == 0x0) {  // continuation
      if (!in_fragment_) throw std::runtime_error("Unexpected continuation frame");
      fragment_payload_.insert(fragment_payload_.end(), payload.begin(), payload.end());
      if (fin) {
        WsFrame f;
        f.opcode = fragment_opcode_;
        f.fin = true;
        f.payload = std::move(fragment_payload_);
        in_fragment_ = false;
        fragment_opcode_ = 0;
        fragment_payload_.clear();
        if (outFrames) outFrames->push_back(std::move(f));
      }
      continue;
    }

    if (opcode == 0x1 || opcode == 0x2) {  // text/binary
      if (in_fragment_) throw std::runtime_error("New data frame while fragmented");
      if (fin) {
        WsFrame f;
        f.opcode = opcode;
        f.fin = true;
        f.payload = std::move(payload);
        if (outFrames) outFrames->push_back(std::move(f));
      } else {
        in_fragment_ = true;
        fragment_opcode_ = opcode;
        fragment_payload_ = std::move(payload);
      }
      continue;
    }

    // Control frames.
    if (opcode == 0x8 || opcode == 0x9 || opcode == 0xA) {
      if (opcode == 0x8 && payload.size() == 1) {
        LOG_WARN("WebSocket", "Close帧负载长度为1字节，协议非法");
        throw std::runtime_error("Invalid close payload length");
      }
      if (outFrames) {
        WsFrame f;
        f.opcode = opcode;
        f.fin = true;
        f.payload = std::move(payload);
        outFrames->push_back(std::move(f));
      }
      continue;
    }

    // Unknown opcode.
    throw std::runtime_error("Unknown opcode");
  }

  // Compact buffer if we consumed any bytes.
  if (parse_offset_ > 0) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(parse_offset_));
    parse_offset_ = 0;
  }

  return producedAccept;
}

