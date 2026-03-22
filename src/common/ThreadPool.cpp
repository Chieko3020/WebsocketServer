#include "common/ThreadPool.h"

/*
 * 文件作用说明：
 * 该文件实现线程池的核心并发逻辑：
 * 1) 构造时创建固定数量工作线程；
 * 2) submit() 将任务压入共享队列；
 * 3) workerLoop() 循环取任务执行；
 * 4) 析构阶段安全停机并回收线程资源。
 */

#include <utility>

ThreadPool::ThreadPool(std::size_t worker_count) {
  if (worker_count == 0) worker_count = 1;
  // 预留容量可减少 vector 扩容开销，避免线程对象移动。
  workers_.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) {
    // 每个线程都执行同一个 workerLoop，持续消费任务队列。
    workers_.emplace_back([this]() { workerLoop(); });
  }
}

ThreadPool::~ThreadPool() {
  {
    // 设置停止标记后，worker 在队列耗尽时退出循环。
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  // 唤醒全部等待线程，让它们感知 stop_ 状态。
  cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
}

void ThreadPool::submit(std::function<void()> fn) {
  {
    // 任务入队与状态访问都在互斥锁保护下进行。
    std::lock_guard<std::mutex> lk(mu_);
    tasks_.push(std::move(fn));
    submitted_.fetch_add(1, std::memory_order_relaxed);
    uint64_t cur = static_cast<uint64_t>(tasks_.size());
    uint64_t old_peak = queue_peak_.load(std::memory_order_relaxed);
    while (cur > old_peak && !queue_peak_.compare_exchange_weak(old_peak, cur, std::memory_order_relaxed)) {
    }
  }
  cv_.notify_one();
  // 唤醒一个等待线程处理刚提交的任务。
}

void ThreadPool::workerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      // 条件等待：当 stop_ 被置位或队列有任务时唤醒。
      cv_.wait(lk, [this]() { return stop_ || !tasks_.empty(); });
      // 若收到停止信号且没有待处理任务，线程安全退出。
      if (stop_ && tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    // 锁外执行任务，避免任务执行时长影响队列并发吞吐。
    if (task) {
      task();
      completed_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

ThreadPool::Stats ThreadPool::stats() {
  Stats s;
  s.submitted = submitted_.load(std::memory_order_relaxed);
  s.completed = completed_.load(std::memory_order_relaxed);
  s.queue_peak = queue_peak_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(mu_);
    s.queue_current = static_cast<uint64_t>(tasks_.size());
  }
  return s;
}

