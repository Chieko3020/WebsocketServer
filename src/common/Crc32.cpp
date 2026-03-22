#include "common/Crc32.h"

/*
 * 文件作用说明：
 * 该文件实现 CRC32（多项式 0xEDB88320）算法。
 * 实现策略：
 * - 首次使用时构建 256 项查找表；
 * - 计算阶段按字节滚动更新；
 * - 最终结果按 CRC32 标准规则进行按位异或收尾。
 */

uint32_t CRC32::table_[256];
bool CRC32::table_init_ = false;

void CRC32::initTable() {
  if (table_init_) return;
  // 预计算查找表：将每个 8bit 输入映射到一次完整“右移+多项式”变换结果。
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int j = 0; j < 8; ++j) {
      if (c & 1) c = 0xEDB88320u ^ (c >> 1);
      else c >>= 1;
    }
    table_[i] = c;
  }
  table_init_ = true;
}

CRC32::CRC32() {
  // 构造时确保查找表可用。
  initTable();
}

uint32_t CRC32::checksum(const uint8_t* data, std::size_t len) const {
  // 0xFFFFFFFF 是 CRC32 的常见初始值。
  uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < len; ++i) {
    crc = table_[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

uint32_t CRC32::checksum(const std::vector<uint8_t>& data) const {
  return checksum(data.data(), data.size());
}

void CRC32::reset() {
  initTable();
  // 流式模式下重置当前累计状态。
  current_ = 0xFFFFFFFFu;
}

void CRC32::update(const uint8_t* data, std::size_t len) {
  // 流式累计：每次传入一个数据块，更新 current_。
  for (std::size_t i = 0; i < len; ++i) {
    current_ = table_[(current_ ^ data[i]) & 0xFFu] ^ (current_ >> 8);
  }
}

uint32_t CRC32::finish() const {
  // 返回最终 CRC32 值（按标准做末尾异或）。
  return current_ ^ 0xFFFFFFFFu;
}

