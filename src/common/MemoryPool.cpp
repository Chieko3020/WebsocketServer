#include "common/MemoryPool.h"

#include "common/Logger.h"

#include <algorithm>
#include <cstring>

MemoryPool::MemoryPool(std::size_t blockSize, std::size_t preallocCount, const std::string& name)
    : blockSize_(blockSize), name_(name) {
  // 预分配：一次性申请 preallocCount 个块，指针全部进入空闲链表。
  ownedBlocks_.reserve(preallocCount);
  freeList_.reserve(preallocCount);
  for (std::size_t i = 0; i < preallocCount; ++i) {
    // unique_ptr 管理 new[]，析构时自动释放整块。
    ownedBlocks_.emplace_back(new uint8_t[blockSize_]);
    // 将裸指针挂入空闲链，供 acquire 快速取出。
    freeList_.push_back(ownedBlocks_.back().get());
  }
  LOG_INFO("内存池", "初始化 name=" + name_ + " blockSize=" + std::to_string(blockSize_) +
                         " 预分配块数=" + std::to_string(preallocCount));
}

MemoryPool::~MemoryPool() {
  // unique_ptr 析构释放 ownedBlocks_；堆回退分配的块应在 release 时已 delete[]。
  // 若仍有未 release 的借用，属于逻辑错误；此处仅清空空闲链。
  std::lock_guard<std::mutex> lk(mu_);
  freeList_.clear();
}

bool MemoryPool::isOwnedPointer(const uint8_t* p) const {
  // 线性扫描 ownedBlocks_：块数量通常为几百，可接受。
  for (const auto& up : ownedBlocks_) {
    if (up.get() == p) return true;
  }
  return false;
}

uint8_t* MemoryPool::acquire() {
  std::lock_guard<std::mutex> lk(mu_);
  if (!freeList_.empty()) {
    uint8_t* p = freeList_.back();
    freeList_.pop_back();
    return p;
  }
  // 空闲链已空：从进程堆借一块，行为与原先 vector 分配一致。
  heap_fallback_total_.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t n = warn_counter_.fetch_add(1, std::memory_order_relaxed);
  if (n % kWarnEvery == 0) {
    LOG_WARN("内存池",
             "name=" + name_ + " 空闲块耗尽，已从堆回退分配；累计回退次数=" +
                 std::to_string(heap_fallback_total_.load(std::memory_order_relaxed)));
  }
  return new uint8_t[blockSize_];
}

void MemoryPool::release(uint8_t* p) {
  if (!p) return;
  std::lock_guard<std::mutex> lk(mu_);
  if (isOwnedPointer(p)) {
    freeList_.push_back(p);
    return;
  }
  delete[] p;
}

std::size_t MemoryPool::freeCount() const {
  std::lock_guard<std::mutex> lk(mu_);
  return freeList_.size();
}

TlsReadBufferGuard::TlsReadBufferGuard(MemoryPool* pool) : pool_(pool) {
  if (!pool_) {
    ptr_ = nullptr;
    size_ = 0;
    return;
  }
  ptr_ = pool_->acquire();
  size_ = pool_->blockSize();
}

TlsReadBufferGuard::~TlsReadBufferGuard() {
  if (pool_ && ptr_) {
    pool_->release(ptr_);
  }
  ptr_ = nullptr;
  pool_ = nullptr;
}
