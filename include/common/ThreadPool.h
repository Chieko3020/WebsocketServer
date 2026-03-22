#pragma once

/*
 * 文件作用说明：
 * 该头文件定义线程池类 ThreadPool，负责将“业务任务”从 IO 线程异步下发到工作线程执行。
 * 在本项目中，线程池是“网络事件循环（epoll）与业务处理（文本回显/文件协议）解耦”的关键。
 */

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
  struct Stats {
    uint64_t submitted{0};
    uint64_t completed{0};
    uint64_t queue_peak{0};
    uint64_t queue_current{0};
  };

  // 构造函数：worker_count 表示预创建的工作线程数。
  explicit ThreadPool(std::size_t worker_count);
  // 析构函数：通知全部线程退出并阻塞等待 join，确保优雅关闭。
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // 提交一个待执行任务到队列末尾，任务类型为无参无返回函数对象。
  void submit(std::function<void()> fn);
  // 获取线程池观测指标快照（提交数/完成数/队列峰值/当前队列长度）。
  Stats stats();

private:
  // 工作线程主循环：阻塞等待任务 -> 取任务 -> 执行任务。
  void workerLoop();

  // 互斥锁：保护任务队列与 stop_ 状态。
  std::mutex mu_;
  // 条件变量：用于任务到达和停机通知。
  std::condition_variable cv_;
  // 任务队列：先进先出，保证提交顺序。
  std::queue<std::function<void()>> tasks_;
  // 停机标记：析构阶段置 true，通知 worker 退出。
  bool stop_{false};
  // 工作线程集合。
  std::vector<std::thread> workers_;

  // 已提交任务总数（累积值）。
  std::atomic<uint64_t> submitted_{0};
  // 已完成任务总数（累积值）。
  std::atomic<uint64_t> completed_{0};
  // 队列历史峰值（用于观察背压和线程池容量匹配度）。
  std::atomic<uint64_t> queue_peak_{0};
};

