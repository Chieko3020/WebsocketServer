#pragma once

/*
 * 文件作用说明：
 * 该头文件定义 WebSocket 协议工具与增量解析器接口。
 * 主要覆盖 RFC6455 的核心要素：
 * - 握手 Accept 计算；
 * - 服务端/客户端帧编码；
 * - 流式（增量）帧解析；
 * - 控制帧与分片帧处理状态。
 */

#include <cstdint>
#include <string>
#include <vector>

struct WsFrame {
  // 帧类型：1=text, 2=binary, 8=close, 9=ping, 10=pong, 0=continuation
  uint8_t opcode{0};
  // 是否为消息的最后一个分片。
  bool fin{true};
  // 解码后的有效负载（已完成 unmask 处理）。
  std::vector<uint8_t> payload;
};

// WebSocket 编码辅助类（RFC6455）。
class WebSocketCodec {
public:
  // 根据客户端 Sec-WebSocket-Key 计算服务端 Sec-WebSocket-Accept。
  static std::string computeAccept(const std::string& secWebSocketKey);

  // 构建服务端发给客户端的帧（不加 mask）。
  static std::vector<uint8_t> buildFrame(uint8_t opcode, const std::vector<uint8_t>& payload,
                                         bool fin = true);
  static std::vector<uint8_t> buildTextFrame(const std::string& text);
  static std::vector<uint8_t> buildBinaryFrame(const std::vector<uint8_t>& data);
  static std::vector<uint8_t> buildPing(const std::vector<uint8_t>& payload);
  static std::vector<uint8_t> buildPong(const std::vector<uint8_t>& payload);
  static std::vector<uint8_t> buildClose(uint16_t statusCode);

  // 构建客户端发给服务端的帧（RFC 要求必须加 mask）。
  static std::vector<uint8_t> buildClientFrame(uint8_t opcode, const std::vector<uint8_t>& payload);
  static std::vector<uint8_t> buildClientTextFrame(const std::string& text);
  static std::vector<uint8_t> buildClientBinaryFrame(const std::vector<uint8_t>& data);
  static std::vector<uint8_t> buildClientPing(const std::vector<uint8_t>& payload);
};

// WebSocket 流式增量解析器：输入任意分段字节流，输出完整帧列表。
class WebSocketStreamParser {
public:
  // 状态机：
  // - AwaitingHttpUpgrade：等待并解析 HTTP Upgrade 请求头
  // - Open：握手完成，可解析 WebSocket 帧
  enum class State {
    AwaitingHttpUpgrade,
    Open
  };

  WebSocketStreamParser();
  explicit WebSocketStreamParser(bool requireMaskFromPeer);

  // 当握手由外部逻辑完成时，可强制切换到 Open 模式（客户端场景常用）。
  void setOpenMode();

  // 读取当前状态（主要用于调试或单元测试断言）。
  State state() const { return state_; }

  // 尝试消费 HTTP 升级请求；成功时生成完整 101 响应。
  bool tryConsumeUpgrade(std::string* outAcceptResponse);

  // 输入新字节并解析：
  // - 若升级响应已就绪，outAcceptResponse 非空；
  // - 若解析出完整帧，写入 outFrames；
  // - 返回值表示本次是否产生升级响应。
  bool feed(const uint8_t* data, std::size_t len, std::string* outAcceptResponse, std::vector<WsFrame>* outFrames);

private:
  // 当前解析状态（等待升级 / 已打开）。
  State state_{State::AwaitingHttpUpgrade};
  // 原始字节缓冲区：累积 TLS 解密后但尚未完全消费的数据。
  std::vector<uint8_t> buffer_;

  // Incremental frame parsing.
  // 本轮解析游标偏移，指向 buffer_ 内已处理边界。
  std::size_t parse_offset_{0};

  // Whether to enforce mask bit on incoming frames.
  // true：要求来向帧必须带掩码（服务端接收客户端帧默认启用）；
  // false：允许不带掩码（客户端接收服务端帧场景）。
  bool require_mask_{true};

  // For fragmented data messages.
  // 当前是否处于“分片消息拼接中”状态。
  bool in_fragment_{false};
  // 分片消息的起始 opcode（文本或二进制）。
  uint8_t fragment_opcode_{0};
  // 已累计的分片 payload。
  std::vector<uint8_t> fragment_payload_;
};

