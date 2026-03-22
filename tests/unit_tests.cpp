#include "common/ConfigFile.h"
#include "common/Crc32.h"
#include "common/MemoryPool.h"
#include "ws/WebSocketCodec.h"

#include <unordered_map>

/*
 * 文件作用说明：
 * 该文件用于最小化回归测试，覆盖本项目最关键、最容易回归出错的基础逻辑：
 * 1) WebSocket 握手 Accept 计算是否符合 RFC 示例；
 * 2) CRC32 实现是否符合标准向量；
 * 3) WebSocket 掩码方向（客户端加掩码、服务端不加掩码）解析是否正确。
 */

#include <cassert>
#include <cstdint>
#include <iostream>

static void testWsAccept() {
  // RFC6455 官方示例：用于验证 Accept 计算正确性。
  std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
  std::string expected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
  std::string accept = WebSocketCodec::computeAccept(key);
  assert(accept == expected);
}

static void testCrc32() {
  // 标准向量："123456789" 的 CRC32 结果应为 0xCBF43926。
  CRC32 crc;
  const char* s = "123456789";
  uint32_t got = crc.checksum(reinterpret_cast<const uint8_t*>(s), 9);
  assert(got == 0xCBF43926u);
}

static void testWsFramesMasking() {
  // 该场景模拟“服务端解析客户端帧”，因此必须要求来向帧带掩码。
  WebSocketStreamParser parser(true);
  parser.setOpenMode();

  auto frameBytes = WebSocketCodec::buildClientTextFrame("hi");

  std::string dummyAccept;
  std::vector<WsFrame> frames;
  bool producedAccept = parser.feed(frameBytes.data(), frameBytes.size(), &dummyAccept, &frames);
  (void)producedAccept;

  assert(frames.size() == 1);
  assert(frames[0].opcode == 0x1);
  assert(frames[0].payload.size() == 2);
  assert(frames[0].payload[0] == 'h');
  assert(frames[0].payload[1] == 'i');
}

static void testConfigGetters() {
  std::unordered_map<std::string, std::string> m;
  m["port"] = "8443";
  m["log_file"] = "false";
  m["ws_idle_timeout"] = "90";
  assert(config_file::getInt(m, "port", 0) == 8443);
  assert(config_file::getBool(m, "log_file", true) == false);
  assert(config_file::getInt(m, "ws_idle_timeout", 0) == 90);
  assert(config_file::getString(m, "missing", "x") == "x");
}

static void testMemoryPool() {
  // 预分配 4 块×64 字节；借出第 5 块应走堆回退。
  MemoryPool pool(64, 4, "unit_test");
  uint8_t* a = pool.acquire();
  uint8_t* b = pool.acquire();
  uint8_t* c = pool.acquire();
  uint8_t* d = pool.acquire();
  assert(pool.freeCount() == 0);
  uint8_t* e = pool.acquire();
  assert(e != nullptr);
  assert(pool.heapFallbackTotal() >= 1u);
  pool.release(e);
  pool.release(a);
  pool.release(b);
  pool.release(c);
  pool.release(d);
  assert(pool.freeCount() == 4);
  TlsReadBufferGuard g(&pool);
  assert(g.data() != nullptr);
  assert(g.size() == 64);
}

static void testWsFramesUnmaskedServerToClient() {
  // 该场景模拟“客户端解析服务端帧”，允许来向帧不带掩码。
  WebSocketStreamParser parser(false);
  parser.setOpenMode();

  auto frameBytes = WebSocketCodec::buildTextFrame("ok");

  std::string dummyAccept;
  std::vector<WsFrame> frames;
  bool producedAccept = parser.feed(frameBytes.data(), frameBytes.size(), &dummyAccept, &frames);
  (void)producedAccept;

  assert(frames.size() == 1);
  assert(frames[0].opcode == 0x1);
  assert(frames[0].payload.size() == 2);
  assert(frames[0].payload[0] == 'o');
  assert(frames[0].payload[1] == 'k');
}

int main() {
  // 调用链：按“握手 -> 校验 -> 掩码方向”顺序执行回归测试。
  testWsAccept();
  testCrc32();
  testConfigGetters();
  testMemoryPool();
  testWsFramesMasking();
  testWsFramesUnmaskedServerToClient();
  std::cout << "unit tests passed\n";
  return 0;
}

