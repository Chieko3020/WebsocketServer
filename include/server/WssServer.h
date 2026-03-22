#pragma once

/*
 * 文件作用说明：
 * 本头文件声明 WssServer 服务端主类。
 * 类职责：
 * - 启动 TLS1.3 + WebSocket 服务器；
 * - 维护 epoll 事件循环（TLS 监听、可选明文 HTTP 监听、定时器、连接 fd 同循环处理）；
 * - 协调连接状态、心跳、业务分发与资源回收。
 */

#include <cstdint>
#include <string>

class WssServer {
public:
  // 默认构造：当前不做重量级初始化，真正初始化在 run() 内完成。
  WssServer();
  // 默认析构：资源主要由 run() 作用域对象自动释放；
  // 若未来引入成员级资源（如后台线程），需在此补充显式回收逻辑。
  ~WssServer();

  // 阻塞启动：函数返回时通常意味着启动失败或服务退出。
  // certFile/keyFile 必须是 PEM 格式证书与私钥文件路径。
  // 返回 true 表示服务正常运行结束（理论上不会主动返回）；
  // 返回 false 表示启动或运行阶段出现异常。
  // httpPort：明文 HTTP 静态服务监听端口（与 TLS/WSS 的 port 相互独立，共享同一 epoll）；
  //           传 0 表示不创建 http_listen_fd，不占用额外端口。
  // httpRoot：磁盘上静态文件根路径（通常为相对 cwd 的 "web"，内含 index.html）；
  //           GET 映射规则见 WssServer.cpp 中 buildHttpStaticResponse。
  // wsIdleTimeoutSec：WebSocket 升级后，若超过该秒数无任何「保活」则关闭连接（≤0 时用默认 120）。
  // wsServerPingIntervalSec：服务端主动发 RFC6455 Ping 的间隔（秒）；≤0 时自动为约 idle/3（至少 5），
  //                           便于浏览器自动回 Pong，避免长时间无操作被误杀。
  // browserClientLogDir：明文 HTTP 端口启用时，浏览器可通过 POST /__wss_browser_log 追加一行到
  //                      「该目录/wss_browser.log」；传空字符串则关闭该端点（返回 503）。
  bool run(uint16_t port, const std::string& certFile, const std::string& keyFile, uint16_t httpPort = 8080,
            const std::string& httpRoot = "web", int wsIdleTimeoutSec = 120, int wsServerPingIntervalSec = 0,
            const std::string& browserClientLogDir = "log");

  // 显式禁用拷贝构造：
  // 服务实例语义上应唯一，避免复制后出现 fd/SSL 上下文等资源二次管理风险。
  WssServer(const WssServer&) = delete;
  // 显式禁用拷贝赋值：
  // 同上，禁止对象间共享/覆盖运行态资源所有权。
  WssServer& operator=(const WssServer&) = delete;
};

