#pragma once

/*
 * 文件作用说明：
 * 定义文件分片传输管理器接口。
 * 该类负责处理 WebSocket 二进制负载中的文件协议消息，
 * 并把响应内容回传给调用者，由上层封装为 WebSocket 二进制帧发送给客户端。
 */

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class FileTransferManager {
public:
  // 构造时会检查/创建上传目录，确保后续文件写入路径可用。
  FileTransferManager();
  // 析构时当前仅依赖标准容器自动回收。
  ~FileTransferManager();

  // 处理文件协议消息：
  // - connId：连接编号（可用于审计/扩展权限控制）
  // - payload：原始应用层消息（即 WS 二进制帧 payload）
  // - outReplies：输出响应列表（每项均为应用层 payload，交由上层封成 WS 帧）
  void handleClientMessage(uint64_t connId, const std::vector<uint8_t>& payload,
                            std::vector<std::vector<uint8_t>>* outReplies);

  // 清理过期文件会话并在会话数量超限时执行淘汰。
  // ttl_seconds：会话空闲超过该阈值即过期。
  // max_sessions：允许保留的最大会话数量，超过后按“最近更新时间最旧优先”淘汰。
  void cleanupSessions(uint64_t now_seconds, uint64_t ttl_seconds, std::size_t max_sessions);

private:
  std::mutex mu_;
};

