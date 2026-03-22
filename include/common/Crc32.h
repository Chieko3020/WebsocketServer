#pragma once

/*
 * 文件作用说明：
 * 该头文件定义 CRC32 校验类，用于：
 * - 大文件分片校验（每个 chunk）；
 * - 整文件最终一致性校验；
 * - 单元测试中的标准向量验证。
 */

#include <cstddef>
#include <cstdint>
#include <vector>

class CRC32 {
public:
  CRC32();

  // 一次性计算：输入连续内存块，返回 CRC32 结果。
  uint32_t checksum(const uint8_t* data, std::size_t len) const;
  // 一次性计算：输入字节向量。
  uint32_t checksum(const std::vector<uint8_t>& data) const;

  // 流式计算：用于分块读取大文件时逐段更新 CRC32。
  void reset();
  void update(const uint8_t* data, std::size_t len);
  uint32_t finish() const;

private:
  // CRC32 查找表（静态共享）：减少重复计算。
  static uint32_t table_[256];
  // 查找表是否完成初始化。
  static bool table_init_;
  // 初始化查找表。
  static void initTable();

  // 流式计算当前累计值。
  uint32_t current_{0xFFFFFFFFu};
};

