#pragma once

/*
 * 文件作用说明：
 * 该文件定义项目统一日志组件接口，用于在服务端、客户端、协议层和文件传输层输出结构化中文日志。
 * 设计目标：
 * 1) 统一日志格式，便于调试跨模块调用链；
 * 2) 提供线程安全输出，避免多线程日志互相覆盖；
 * 3) 支持模块名与日志级别，快速定位问题来源；
 * 4) 代码层面尽量轻量，避免引入第三方日志依赖。
 */

#include <fstream>
#include <memory>
#include <mutex>
#include <string>

enum class LogLevel {
  Debug,  // 调试信息，通常用于细粒度流程追踪
  Info,   // 关键流程信息，默认重点关注
  Warn,   // 非致命异常或可恢复问题
  Error   // 错误信息，通常意味着当前操作失败
};

class Logger {
public:
  // 设置最低输出级别：低于该级别的日志将被过滤。
  static void setMinLevel(LogLevel level);

  // 初始化文件日志：在 log_dir 下创建目录（若不存在），并追加写入 wss_server.log。
  // 与标准错误输出并行；失败时返回 false（仍可向 stderr 打日志）。
  static bool initFileLog(const std::string& log_dir);
  // 在 log_dir 下追加写入指定文件名（如 wss_client.log），格式与 initFileLog 相同；与 stderr 并行。
  // 若已有打开的文件句柄会先关闭再打开新文件（进程内仅保留一个文件目标）。
  static bool initFileLogWithName(const std::string& log_dir, const std::string& filename);
  // 关闭文件日志句柄（进程退出前可选调用）。
  static void shutdownFileLog();

  // 统一日志输出入口。
  static void log(LogLevel level, const std::string& module, const std::string& message);

private:
  // 将枚举日志级别映射为中文文本。
  static const char* levelToChinese(LogLevel level);
  // 获取当前本地时间字符串（精确到毫秒）。
  static std::string nowString();

  // 全局互斥锁，确保并发日志输出不交错。
  static std::mutex mu_;
  // 最低日志级别阈值。
  static LogLevel min_level_;
  // 可选：追加写入的日志文件（与 stderr 并行）。
  static std::unique_ptr<std::ofstream> file_out_;
};

// 便捷宏：调用时只需提供模块与中文消息。
#define LOG_DEBUG(module, message) Logger::log(LogLevel::Debug, module, message)
#define LOG_INFO(module, message) Logger::log(LogLevel::Info, module, message)
#define LOG_WARN(module, message) Logger::log(LogLevel::Warn, module, message)
#define LOG_ERROR(module, message) Logger::log(LogLevel::Error, module, message)

