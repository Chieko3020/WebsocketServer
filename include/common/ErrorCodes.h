#pragma once

/*
 * 文件作用说明：
 * 本文件统一定义项目错误码与错误响应构造工具，确保：
 * 1) 服务端各模块使用统一错误语义；
 * 2) 客户端可稳定解析并输出中文解释；
 * 3) 文档中的错误码表与代码实现保持一致。
 */

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace error_codes {

enum class Code : uint16_t {
  // 通用错误（0x1000）
  INVALID_ARGUMENT = 0x1001,
  INTERNAL_ERROR = 0x1002,
  UNSUPPORTED_OPCODE = 0x1003,
  PROTOCOL_VIOLATION = 0x1004,

  // WebSocket 错误（0x2000）
  WS_INVALID_FRAME = 0x2001,
  WS_CONTROL_FRAME_TOO_LARGE = 0x2002,
  WS_INVALID_CLOSE_CODE = 0x2003,
  WS_PAYLOAD_TOO_LARGE = 0x2004,

  // 文件协议错误（0x3000）
  FILE_STATE_NOT_FOUND = 0x3001,
  FILE_CHUNK_CRC_MISMATCH = 0x3002,
  FILE_CHUNK_OUT_OF_RANGE = 0x3003,
  FILE_SESSION_EXPIRED = 0x3004,
};

inline std::string toChinese(Code code) {
  switch (code) {
    case Code::INVALID_ARGUMENT: return "参数非法";
    case Code::INTERNAL_ERROR: return "内部错误";
    case Code::UNSUPPORTED_OPCODE: return "不支持的消息类型";
    case Code::PROTOCOL_VIOLATION: return "协议违规";
    case Code::WS_INVALID_FRAME: return "WebSocket帧非法";
    case Code::WS_CONTROL_FRAME_TOO_LARGE: return "WebSocket控制帧超长";
    case Code::WS_INVALID_CLOSE_CODE: return "WebSocket关闭码非法";
    case Code::WS_PAYLOAD_TOO_LARGE: return "WebSocket负载过大";
    case Code::FILE_STATE_NOT_FOUND: return "文件会话不存在";
    case Code::FILE_CHUNK_CRC_MISMATCH: return "文件分片CRC校验失败";
    case Code::FILE_CHUNK_OUT_OF_RANGE: return "文件分片序号越界";
    case Code::FILE_SESSION_EXPIRED: return "文件会话已过期";
    default: return "未知错误";
  }
}

inline std::string buildTextError(Code code, const std::string& detail) {
  std::ostringstream oss;
  oss << "{\"type\":\"error\",\"code\":" << static_cast<uint16_t>(code)
      << ",\"message\":\"" << toChinese(code) << "\",\"detail\":\"" << detail << "\"}";
  return oss.str();
}

inline void appendU16BE(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(value & 0xFFu));
}

inline uint16_t readU16BE(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

// 二进制错误包格式：
// [0xEE][code_hi][code_lo][utf8_detail...]
inline std::vector<uint8_t> buildBinaryError(Code code, const std::string& detail) {
  std::vector<uint8_t> out;
  out.push_back(0xEE);
  appendU16BE(&out, static_cast<uint16_t>(code));
  out.insert(out.end(), detail.begin(), detail.end());
  return out;
}

}  // namespace error_codes

