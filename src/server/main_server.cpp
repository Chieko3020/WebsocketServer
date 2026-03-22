#include "server/WssServer.h"

/*
 * 文件作用说明：
 * 服务端程序入口文件。
 * 负责解析配置文件（config/）与命令行参数并调用 WssServer::run() 启动主服务循环。
 * 优先级：命令行 > 配置文件 > 代码内置默认值。
 */

#include "common/ConfigFile.h"
#include "common/Logger.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog
            << " [--config <path>] "
               "[--port <port>] [--cert <cert.pem>] [--key <key.pem>] "
               "[--enable-ticket <0|1>] [--session-timeout <sec>] [--enable-0rtt <0|1>] "
               "[--http-port <port|0>] [--http-root <dir>] "
               "[--log-dir <dir>] [--no-log-file] "
               "[--ws-idle-timeout <sec>] [--ws-ping-interval <sec>]\n"
            << "\n默认读取配置文件: config/wss_server.conf（若存在）。\n"
            << "端口、证书等可在配置文件中预置，不必全部写在命令行。\n";
}

static bool fileExists(const std::string& path) {
  std::ifstream f(path.c_str());
  return f.good();
}

int main(int argc, char** argv) {
  Logger::setMinLevel(LogLevel::Info);

  // —— 第一遍：仅解析 --config，默认 config/wss_server.conf ——
  std::string configPath = "config/wss_server.conf";
  bool userSetConfigPath = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cerr << "[错误] --config 需要指定文件路径\n";
        return 2;
      }
      configPath = argv[++i];
      userSetConfigPath = true;
    }
  }

  std::unordered_map<std::string, std::string> cfg;
  std::string loadErr;
  bool loaded = false;
  if (fileExists(configPath)) {
    if (!config_file::load(configPath, &cfg, &loadErr)) {
      std::cerr << "[错误] 配置文件解析失败: " << configPath << " — " << loadErr << "\n";
      return 2;
    }
    loaded = true;
  } else if (userSetConfigPath) {
    std::cerr << "[错误] 找不到配置文件: " << configPath << "\n";
    return 2;
  }

  // —— 自配置文件注入默认值（命令行随后覆盖）——
  uint16_t port = static_cast<uint16_t>(config_file::getInt(cfg, "port", 0));
  std::string certFile = config_file::getString(cfg, "cert", "");
  std::string keyFile = config_file::getString(cfg, "key", "");
  int enableTicket = config_file::getInt(cfg, "enable_ticket", 1);
  long sessionTimeout = config_file::getLong(cfg, "session_timeout", 300);
  int enable0Rtt = config_file::getInt(cfg, "enable_0rtt", 0);
  uint16_t httpPort = static_cast<uint16_t>(config_file::getInt(cfg, "http_port", 8080));
  std::string httpRoot = config_file::getString(cfg, "http_root", "web");
  std::string logDir = config_file::getString(cfg, "log_dir", "log");
  bool enableFileLog = config_file::getBool(cfg, "log_file", true);
  int wsIdleTimeoutSec = config_file::getInt(cfg, "ws_idle_timeout", 0);
  int wsServerPingIntervalSec = config_file::getInt(cfg, "ws_ping_interval", 0);

  // —— 第二遍：命令行覆盖 ——
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      ++i;
      continue;
    }
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--cert" && i + 1 < argc) {
      certFile = argv[++i];
    } else if (arg == "--key" && i + 1 < argc) {
      keyFile = argv[++i];
    } else if (arg == "--enable-ticket" && i + 1 < argc) {
      enableTicket = std::atoi(argv[++i]);
    } else if (arg == "--session-timeout" && i + 1 < argc) {
      sessionTimeout = std::atol(argv[++i]);
    } else if (arg == "--enable-0rtt" && i + 1 < argc) {
      enable0Rtt = std::atoi(argv[++i]);
    } else if (arg == "--http-port" && i + 1 < argc) {
      httpPort = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--http-root" && i + 1 < argc) {
      httpRoot = argv[++i];
    } else if (arg == "--log-dir" && i + 1 < argc) {
      logDir = argv[++i];
    } else if (arg == "--no-log-file") {
      enableFileLog = false;
    } else if (arg == "--ws-idle-timeout" && i + 1 < argc) {
      wsIdleTimeoutSec = std::atoi(argv[++i]);
    } else if (arg == "--ws-ping-interval" && i + 1 < argc) {
      wsServerPingIntervalSec = std::atoi(argv[++i]);
    } else {
      std::cerr << "[错误] 未知参数: " << arg << "\n";
      usage(argv[0]);
      return 2;
    }
  }

  if (port == 0 || certFile.empty() || keyFile.empty()) {
    usage(argv[0]);
    return 2;
  }

  if (loaded) {
    LOG_INFO("入口", "已加载配置文件: " + configPath);
  }

  if (enableFileLog) {
    if (!Logger::initFileLog(logDir)) {
      std::cerr << "[警告] 文件日志初始化失败（目录: " << logDir
                << "），仅使用标准错误输出。\n";
    }
  }

  LOG_INFO("入口", "启动参数解析完成，WSS端口=" + std::to_string(port) +
                       "，内置HTTP=" + (httpPort == 0 ? std::string("关闭") : ("端口=" + std::to_string(httpPort))) +
                       "，静态根目录=" + httpRoot +
                       "，0-RTT实验=" + (enable0Rtt == 0 ? std::string("关闭") : std::string("开启")));

  setenv("WSS_ENABLE_TICKET", enableTicket == 0 ? "0" : "1", 1);
  setenv("WSS_SESSION_TIMEOUT", std::to_string(sessionTimeout).c_str(), 1);
  setenv("WSS_ENABLE_0RTT", enable0Rtt == 0 ? "0" : "1", 1);

  WssServer server;
  // log_dir：Logger 写 wss_server.log（可 --no-log-file 关）；浏览器 POST /__wss_browser_log 仍用同目录写 wss_browser.log。
  if (!server.run(port, certFile, keyFile, httpPort, httpRoot, wsIdleTimeoutSec, wsServerPingIntervalSec, logDir))
    return 1;
  return 0;
}
