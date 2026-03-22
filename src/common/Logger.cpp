#include "common/Logger.h"

/*
 * 文件作用说明：
 * 该文件实现 Logger 的核心行为，包括：
 * - 时间戳生成；
 * - 日志级别过滤；
 * - 线程安全控制；
 * - 统一中文格式化输出。
 */

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>

std::mutex Logger::mu_;
LogLevel Logger::min_level_ = LogLevel::Info;
std::unique_ptr<std::ofstream> Logger::file_out_;

namespace {

bool ensureDir(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  if (::mkdir(path.c_str(), 0755) == 0) return true;
  return errno == EEXIST;
}

}  // namespace

bool Logger::initFileLogWithName(const std::string& log_dir, const std::string& filename) {
  std::lock_guard<std::mutex> lk(mu_);
  file_out_.reset();
  if (log_dir.empty() || filename.empty()) return false;
  if (!ensureDir(log_dir)) return false;
  const std::string path = log_dir + "/" + filename;
  auto out = std::unique_ptr<std::ofstream>(new std::ofstream(path.c_str(), std::ios::app | std::ios::out));
  if (!out->good()) return false;
  file_out_ = std::move(out);
  return true;
}

bool Logger::initFileLog(const std::string& log_dir) {
  return initFileLogWithName(log_dir, "wss_server.log");
}

void Logger::shutdownFileLog() {
  std::lock_guard<std::mutex> lk(mu_);
  file_out_.reset();
}

void Logger::setMinLevel(LogLevel level) {
  // 修改日志阈值时加锁，确保并发安全。
  std::lock_guard<std::mutex> lk(mu_);
  min_level_ = level;
}

const char* Logger::levelToChinese(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "调试";
    case LogLevel::Info: return "信息";
    case LogLevel::Warn: return "警告";
    case LogLevel::Error: return "错误";
    default: return "未知";
  }
}

std::string Logger::nowString() {
  // 取系统时间并格式化为“年月日 时分秒.毫秒”。
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::tm tm_buf;
  localtime_r(&t, &tm_buf);

  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
      << "." << std::setw(3) << std::setfill('0') << ms.count();
  return oss.str();
}

void Logger::log(LogLevel level, const std::string& module, const std::string& message) {
  std::lock_guard<std::mutex> lk(mu_);
  // 级别过滤：低于阈值直接返回，减少无效日志开销。
  if (static_cast<int>(level) < static_cast<int>(min_level_)) return;
  // 输出规范统一为“tag:message”风格：
  // - tag 使用模块名（module）；
  // - message 使用调用方传入文本；
  // - 同时保留时间与级别前缀，便于检索与排障。
  std::ostringstream line;
  line << "[" << nowString() << "]"
       << "[" << levelToChinese(level) << "]"
       << " " << module << ":" << message << "\n";
  const std::string s = line.str();
  std::cerr << s;
  if (file_out_ && file_out_->good()) {
    *file_out_ << s;
    file_out_->flush();
  }
}

