#pragma once

/*
 * 文件作用说明：
 * 本头文件定义固定块大小的简易内存池（Slab），用于减少 TLS 读路径上
 * 反复分配 std::vector 带来的堆开销与碎片。
 *
 * 设计要点：
 * 1) 预分配若干块等长内存，通过空闲链表复用；
 * 2) 池耗尽时回退到堆上 new[]，并打 WARN 日志（可节流）；
 * 3) 线程安全：acquire/release 使用互斥锁；
 * 4) 配合 TlsReadBufferGuard 做 RAII，避免异常路径泄漏。
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class MemoryPool {
public:
  // blockSize：每块字节数（须与调用方 SSL_read 缓冲长度一致）。
  // preallocCount：启动时预分配并放入空闲链的块数量。
  // name：日志中使用的池名称。
  MemoryPool(std::size_t blockSize, std::size_t preallocCount, const std::string& name);

  // 禁止拷贝：池为独占资源。
  MemoryPool(const MemoryPool&) = delete;
  MemoryPool& operator=(const MemoryPool&) = delete;

  // 允许移动（可选，当前未使用）。
  MemoryPool(MemoryPool&&) noexcept = default;
  MemoryPool& operator=(MemoryPool&&) noexcept = default;

  ~MemoryPool();

  // 借出一块缓冲区；保证非 nullptr。池空时从堆分配并累计 fallback 计数。
  uint8_t* acquire();

  // 归还缓冲区：若为本池拥有的块则入空闲链表，否则 delete[]。
  void release(uint8_t* p);

  // 单块容量（字节）。
  std::size_t blockSize() const { return blockSize_; }

  // 统计：预分配块数、当前空闲数、累计堆回退次数。
  std::size_t preallocatedCount() const { return ownedBlocks_.size(); }
  std::size_t freeCount() const;
  std::uint64_t heapFallbackTotal() const { return heap_fallback_total_.load(); }

private:
  // 判断 p 是否指向本池预分配的一块（用于 release 分支）。
  bool isOwnedPointer(const uint8_t* p) const;

  std::size_t blockSize_{0};
  std::string name_;

  // 预分配的大块：每个 unique_ptr 管理 blockSize_ 字节。
  std::vector<std::unique_ptr<uint8_t[]>> ownedBlocks_;
  // 当前空闲的可复用指针（均指向 ownedBlocks_ 中某块）。
  std::vector<uint8_t*> freeList_;

  mutable std::mutex mu_;
  // 累计从堆借出的次数（用于日志与单测）。
  std::atomic<std::uint64_t> heap_fallback_total_{0};
  // WARN 节流：每 WARN_EVERY 次堆回退打一次日志。
  std::atomic<std::uint64_t> warn_counter_{0};
  static constexpr std::uint64_t kWarnEvery = 64;
};

// RAII：构造时 acquire，析构时 release；用于 WssServer::SSL_read 单次循环。
class TlsReadBufferGuard {
public:
  explicit TlsReadBufferGuard(MemoryPool* pool);
  ~TlsReadBufferGuard();

  TlsReadBufferGuard(const TlsReadBufferGuard&) = delete;
  TlsReadBufferGuard& operator=(const TlsReadBufferGuard&) = delete;

  uint8_t* data() { return ptr_; }
  std::size_t size() const { return size_; }

private:
  MemoryPool* pool_{nullptr};
  uint8_t* ptr_{nullptr};
  std::size_t size_{0};
};
