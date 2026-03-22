#include "file/FileTransferManager.h"

#include "common/Crc32.h"
#include "common/ErrorCodes.h"
#include "common/Logger.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstring>
#include <unistd.h>

namespace {

/*
 * 文件作用说明：
 * 该文件实现“大文件分片传输与断点续传”应用层协议。
 * 主要逻辑：
 * - 接收 FILE_START 创建/复用文件会话；
 * - 接收 FILE_QUERY 返回已接收分片位图；
 * - 接收 FILE_CHUNK 校验并写入对应偏移；
 * - 全部分片完成后执行整文件 CRC32 校验并落盘。
 */

constexpr uint8_t MSG_FILE_START = 1;
constexpr uint8_t MSG_FILE_QUERY = 2;
constexpr uint8_t MSG_FILE_CHUNK = 3;

constexpr uint8_t MSG_FILE_QUERY_RESPONSE = 101;
constexpr uint8_t MSG_FILE_CHUNK_ACK = 102;
constexpr uint8_t MSG_FILE_FINISH_ACK = 103;
constexpr uint8_t MSG_FILE_ERROR = 255;

static const char* kUploadsDir = "uploads";

// 工具函数：读取网络字节序 u16。
static uint16_t readU16BE(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

// 工具函数：读取网络字节序 u32。
static uint32_t readU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// 工具函数：读取网络字节序 u64。
static uint64_t readU64BE(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | static_cast<uint64_t>(p[i]);
  }
  return v;
}

// 工具函数：写入网络字节序 u32。
static void writeU32BE(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(v & 0xFFu));
}

// 工具函数：写入网络字节序 u64。
static void writeU64BE(std::vector<uint8_t>* out, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
  }
}

// 工具函数：生成临时文件路径。
static std::string makeTempPath(uint64_t fileId) {
  // For this graduation demo, a deterministic temp file name is OK.
  std::ostringstream oss;
  oss << kUploadsDir << "/tmp_" << fileId << ".bin";
  return oss.str();
}

// 工具函数：仅保留路径最后一段作为文件名，去掉路径穿越风险。
static std::string basenameOnly(const std::string& raw) {
  std::size_t pos = raw.find_last_of("/\\");
  if (pos == std::string::npos) return raw;
  return raw.substr(pos + 1);
}

// 工具函数：清洗客户端传来的原始文件名（UTF-8），用于落盘展示。
static std::string sanitizeOriginalFilename(const std::string& raw) {
  std::string base = basenameOnly(raw);
  while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();
  if (base.empty() || base == "." || base == "..") return "";

  std::string out;
  constexpr std::size_t kMax = 200;
  out.reserve(std::min<std::size_t>(base.size(), kMax));
  for (unsigned char c : base) {
    if (out.size() >= kMax) break;
    if (c < 32) continue;
    if (c == '/' || c == '\\' || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '*' ||
        c == '?')
      continue;
    out.push_back(static_cast<char>(c));
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out;
}

// 工具函数：生成最终落盘文件路径（带原始文件名时保留扩展名等信息）。
static std::string makeFinalPath(uint64_t fileId, const std::string& sanitized_name) {
  std::ostringstream oss;
  oss << kUploadsDir << "/";
  if (sanitized_name.empty()) {
    oss << fileId << ".bin";
  } else {
    oss << fileId << "_" << sanitized_name;
  }
  return oss.str();
}

// 工具函数：确保上传目录存在。
static void ensureUploadsDir() {
  struct stat st;
  if (::stat(kUploadsDir, &st) == 0) {
    if (S_ISDIR(st.st_mode)) return;
    throw std::runtime_error("uploads path exists but is not a directory");
  }
  if (::mkdir(kUploadsDir, 0755) < 0) {
    if (errno == EEXIST) return;
    throw std::runtime_error("mkdir uploads failed");
  }
}

struct FileState {
  uint64_t file_id{0};
  uint64_t file_size{0};
  uint32_t chunk_size{0};
  uint32_t total_chunks{0};
  uint32_t expected_crc32{0};

  std::vector<uint8_t> bitmap;  // 1 bit per chunk.
  uint32_t received_count{0};

  int fd{-1};
  std::string temp_path;
  std::string final_display_name;  // 清洗后的原始文件名；空则落盘为 file_id.bin
  uint64_t create_ts{0};
  uint64_t last_update_ts{0};
};

static std::unordered_map<uint64_t, FileState> g_states;

// 工具函数：读取 bitmap 指定位是否已接收。
static bool bitmapGet(const std::vector<uint8_t>& bm, uint32_t idx) {
  const uint32_t byte = idx / 8;
  const uint32_t bit = idx % 8;
  return ((bm[byte] >> bit) & 0x1u) != 0;
}

// 工具函数：设置 bitmap 指定位为已接收。
static void bitmapSet(std::vector<uint8_t>* bm, uint32_t idx) {
  const uint32_t byte = idx / 8;
  const uint32_t bit = idx % 8;
  (*bm)[byte] = static_cast<uint8_t>((*bm)[byte] | (1u << bit));
}

// 工具函数：基于文件描述符计算整文件 CRC32。
static uint32_t computeFdCrc32(int fd) {
  if (fd < 0) throw std::runtime_error("invalid fd");
  if (::lseek(fd, 0, SEEK_SET) < 0) throw std::runtime_error("lseek failed");

  CRC32 crc;
  crc.reset();

  std::vector<uint8_t> buf(64 * 1024);
  while (true) {
    ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("read failed for crc32");
    }
    if (n == 0) break;
    crc.update(buf.data(), static_cast<std::size_t>(n));
  }

  return crc.finish();
}

}  // namespace

FileTransferManager::FileTransferManager() {
  // 构造时做目录准备，避免首次写文件失败。
  ensureUploadsDir();
  LOG_INFO("文件模块", "上传目录检查完成：uploads/");
}

FileTransferManager::~FileTransferManager() = default;

void FileTransferManager::handleClientMessage(uint64_t /*connId*/, const std::vector<uint8_t>& payload,
                                               std::vector<std::vector<uint8_t>>* outReplies) {
  // 文件协议统一入口：按 msgType 分发到 START/QUERY/CHUNK 三条路径。
  if (!outReplies) return;
  outReplies->clear();
  if (payload.empty()) return;

  std::lock_guard<std::mutex> lk(mu_);

  try {
    uint64_t now_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    uint8_t msgType = payload[0];
    const uint8_t* p = payload.data() + 1;
    const std::size_t remain = payload.size() - 1;

    auto mkError = [&](error_codes::Code code, const std::string& reason) {
      auto r = error_codes::buildBinaryError(code, reason);
      // 兼容旧协议：首字节改写为 FILE_ERROR。
      r[0] = MSG_FILE_ERROR;
      outReplies->push_back(std::move(r));
    };

    if (msgType == MSG_FILE_START) {
      LOG_DEBUG("文件模块", "收到 FILE_START 消息");
      // Layout:
      // 1  type
      // 8  file_id
      // 8  file_size
      // 4  chunk_size
      // 4  total_chunks
      // 4  expected_crc32
      if (remain < 8 + 8 + 4 + 4 + 4) {
        mkError(error_codes::Code::INVALID_ARGUMENT, "FILE_START payload too small");
        return;
      }
      const uint64_t file_id = readU64BE(p);
      const uint64_t file_size = readU64BE(p + 8);
      const uint32_t chunk_size = readU32BE(p + 16);
      const uint32_t total_chunks = readU32BE(p + 20);
      const uint32_t expected_crc32 = readU32BE(p + 24);

      // 可选扩展：uint16 filename_len_be + UTF-8 bytes（与旧版 28 字节固定负载兼容）。
      std::string raw_filename;
      if (remain > 28) {
        if (remain < 30) {
          mkError(error_codes::Code::INVALID_ARGUMENT, "FILE_START filename header incomplete");
          return;
        }
        const uint16_t fn_len = readU16BE(p + 28);
        constexpr uint16_t kMaxFn = 512;
        if (fn_len > kMaxFn) {
          mkError(error_codes::Code::INVALID_ARGUMENT, "FILE_START filename too long");
          return;
        }
        if (remain < 30u + static_cast<std::size_t>(fn_len)) {
          mkError(error_codes::Code::INVALID_ARGUMENT, "FILE_START filename truncated");
          return;
        }
        raw_filename.assign(reinterpret_cast<const char*>(p + 30), static_cast<std::size_t>(fn_len));
      }

      if (total_chunks == 0 || chunk_size == 0) {
        mkError(error_codes::Code::INVALID_ARGUMENT, "invalid chunk params");
        LOG_WARN("文件模块", "FILE_START 参数非法：chunk_size或total_chunks为0");
        return;
      }

      // Reuse previous state if parameters match (enable resumable transfers).
      auto it = g_states.find(file_id);
      if (it != g_states.end()) {
        const FileState& ex = it->second;
        if (ex.file_size == file_size && ex.chunk_size == chunk_size && ex.total_chunks == total_chunks &&
            ex.expected_crc32 == expected_crc32) {
          LOG_INFO("文件模块", "命中断点续传会话，file_id=" + std::to_string(file_id));
          const FileState& st = it->second;
          std::size_t bitmapBytes = st.bitmap.size();
          std::vector<uint8_t> resp;
          resp.push_back(MSG_FILE_QUERY_RESPONSE);
          writeU64BE(&resp, file_id);
          writeU32BE(&resp, total_chunks);
          writeU32BE(&resp, static_cast<uint32_t>(bitmapBytes));
          resp.insert(resp.end(), st.bitmap.begin(), st.bitmap.end());
          outReplies->push_back(std::move(resp));
          g_states[file_id].last_update_ts = now_seconds;
          return;
        }
        if (it->second.fd >= 0) ::close(it->second.fd);
        g_states.erase(it);
      }

      FileState st;
      st.file_id = file_id;
      st.file_size = file_size;
      st.chunk_size = chunk_size;
      st.total_chunks = total_chunks;
      st.expected_crc32 = expected_crc32;
      st.temp_path = makeTempPath(file_id);
      st.final_display_name = sanitizeOriginalFilename(raw_filename);

      // Create/truncate temp file.
      int fd = ::open(st.temp_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
      if (fd < 0) {
        mkError(error_codes::Code::INTERNAL_ERROR, "open temp file failed");
        LOG_ERROR("文件模块", "创建临时文件失败，file_id=" + std::to_string(file_id));
        return;
      }
      if (::ftruncate(fd, static_cast<off_t>(file_size)) < 0) {
        ::close(fd);
        mkError(error_codes::Code::INTERNAL_ERROR, "ftruncate failed");
        return;
      }

      st.fd = fd;
      std::size_t bitmapBytes = (static_cast<std::size_t>(total_chunks) + 7) / 8;
      st.bitmap.assign(bitmapBytes, 0);
      st.received_count = 0;
      st.create_ts = now_seconds;
      st.last_update_ts = now_seconds;

      {
        auto ir = g_states.emplace(file_id, std::move(st));
        FileState& inserted = ir.first->second;
        std::string msg = "新建文件会话成功，file_id=" + std::to_string(file_id) +
                          "，分片总数=" + std::to_string(total_chunks);
        if (!inserted.final_display_name.empty()) {
          msg += "，原始文件名=" + inserted.final_display_name;
        }
        LOG_INFO("文件模块", msg);
      }

      // Reply with "query_response" semantics: all missing (all 0).
      std::vector<uint8_t> resp;
      resp.push_back(MSG_FILE_QUERY_RESPONSE);
      writeU64BE(&resp, file_id);
      writeU32BE(&resp, total_chunks);
      writeU32BE(&resp, static_cast<uint32_t>(bitmapBytes));
      resp.insert(resp.end(), g_states[file_id].bitmap.begin(), g_states[file_id].bitmap.end());
      outReplies->push_back(std::move(resp));
      return;
    }

    if (msgType == MSG_FILE_QUERY) {
      LOG_DEBUG("文件模块", "收到 FILE_QUERY 消息");
      // Layout: type(1) + file_id(8)
      if (remain < 8) {
        mkError(error_codes::Code::INVALID_ARGUMENT, "FILE_QUERY payload too small");
        return;
      }
      const uint64_t file_id = readU64BE(p);
      auto it = g_states.find(file_id);
      if (it == g_states.end()) {
        mkError(error_codes::Code::FILE_STATE_NOT_FOUND, "file state not found");
        LOG_WARN("文件模块", "查询失败：file_id不存在");
        return;
      }

      const FileState& st = it->second;
      std::vector<uint8_t> resp;
      resp.push_back(MSG_FILE_QUERY_RESPONSE);
      writeU64BE(&resp, file_id);
      writeU32BE(&resp, st.total_chunks);
      writeU32BE(&resp, static_cast<uint32_t>(st.bitmap.size()));
      resp.insert(resp.end(), st.bitmap.begin(), st.bitmap.end());
      outReplies->push_back(std::move(resp));
      g_states[file_id].last_update_ts = now_seconds;
      return;
    }

    if (msgType == MSG_FILE_CHUNK) {
      LOG_DEBUG("文件模块", "收到 FILE_CHUNK 消息");
      // Layout:
      // type(1)
      // file_id(8)
      // chunk_index(4)
      // chunk_crc32(4)
      // data(variable)
      if (remain < 8 + 4 + 4) {
        mkError(error_codes::Code::INVALID_ARGUMENT, "FILE_CHUNK payload too small");
        return;
      }
      const uint64_t file_id = readU64BE(p);
      const uint32_t chunk_index = readU32BE(p + 8);
      const uint32_t chunk_crc32 = readU32BE(p + 12);

      auto it = g_states.find(file_id);
      if (it == g_states.end()) {
        mkError(error_codes::Code::FILE_STATE_NOT_FOUND, "file state not found");
        return;
      }

      FileState& st = it->second;
      if (chunk_index >= st.total_chunks) {
        mkError(error_codes::Code::FILE_CHUNK_OUT_OF_RANGE, "chunk_index out of range");
        return;
      }

      const uint8_t* chunkData = p + 16;
      const std::size_t chunkLen = remain - 8 - 4 - 4;
      if (chunkLen == 0) {
        mkError(error_codes::Code::INVALID_ARGUMENT, "empty chunk");
        return;
      }

      // Validate expected size for non-last chunks.
      if (chunk_index + 1 != st.total_chunks && chunkLen != st.chunk_size) {
        // Allow last chunk smaller; others must match.
        mkError(error_codes::Code::PROTOCOL_VIOLATION, "chunk size mismatch");
        return;
      }

      CRC32 crc;
      uint32_t got = crc.checksum(chunkData, chunkLen);
      if (got != chunk_crc32) {
        mkError(error_codes::Code::FILE_CHUNK_CRC_MISMATCH, "chunk crc32 mismatch");
        LOG_WARN("文件模块", "分片CRC校验失败，file_id=" + std::to_string(file_id) +
                             "，chunk=" + std::to_string(chunk_index));
        // Still reply ack with failure for easier debugging.
        std::vector<uint8_t> resp;
        resp.push_back(MSG_FILE_CHUNK_ACK);
        writeU64BE(&resp, file_id);
        writeU32BE(&resp, chunk_index);
        resp.push_back(0);
        outReplies->push_back(std::move(resp));
        return;
      }

      const off_t offset = static_cast<off_t>(chunk_index) * static_cast<off_t>(st.chunk_size);
      ssize_t written = ::pwrite(st.fd, chunkData, chunkLen, offset);
      if (written < 0 || static_cast<std::size_t>(written) != chunkLen) {
        mkError(error_codes::Code::INTERNAL_ERROR, "pwrite failed");
        return;
      }

      bool already = bitmapGet(st.bitmap, chunk_index);
      if (!already) {
        bitmapSet(&st.bitmap, chunk_index);
        st.received_count++;
        st.last_update_ts = now_seconds;
        LOG_DEBUG("文件模块", "分片写入成功，file_id=" + std::to_string(file_id) +
                              "，chunk=" + std::to_string(chunk_index) +
                              "，已收=" + std::to_string(st.received_count) +
                              "/" + std::to_string(st.total_chunks));
      }

      // Ack for this chunk.
      {
        std::vector<uint8_t> resp;
        resp.push_back(MSG_FILE_CHUNK_ACK);
        writeU64BE(&resp, file_id);
        writeU32BE(&resp, chunk_index);
        resp.push_back(already ? 0 : 1);
        outReplies->push_back(std::move(resp));
      }

      if (st.received_count == st.total_chunks) {
        // Completed: verify whole-file CRC.
        uint32_t fileCrc = computeFdCrc32(st.fd);
        bool ok = (fileCrc == st.expected_crc32);
        if (ok) {
          ::close(st.fd);
          st.fd = -1;
          const std::string finalPath = makeFinalPath(st.file_id, st.final_display_name);
          ::rename(st.temp_path.c_str(), finalPath.c_str());
          std::vector<uint8_t> resp;
          resp.push_back(MSG_FILE_FINISH_ACK);
          writeU64BE(&resp, file_id);
          resp.push_back(1);
          outReplies->push_back(std::move(resp));
          LOG_INFO("文件模块", "文件传输完成并校验通过，file_id=" + std::to_string(file_id) +
                                    "，落盘路径=" + finalPath);
        } else {
          ::close(st.fd);
          st.fd = -1;
          ::unlink(st.temp_path.c_str());
          std::vector<uint8_t> resp;
          resp.push_back(MSG_FILE_FINISH_ACK);
          writeU64BE(&resp, file_id);
          resp.push_back(0);
          outReplies->push_back(std::move(resp));
          LOG_ERROR("文件模块", "文件传输完成但整文件CRC校验失败，file_id=" + std::to_string(file_id));
        }
        g_states.erase(file_id);
      }
      return;
    }

    // Unknown message type.
    mkError(error_codes::Code::UNSUPPORTED_OPCODE, "unknown msgType");
    LOG_WARN("文件模块", "收到未知消息类型，msgType=" + std::to_string(msgType));
  } catch (const std::exception& ex) {
    std::vector<uint8_t> resp;
    resp = error_codes::buildBinaryError(error_codes::Code::INTERNAL_ERROR, ex.what());
    resp[0] = MSG_FILE_ERROR;
    outReplies->push_back(std::move(resp));
    LOG_ERROR("文件模块", std::string("处理消息发生异常：") + ex.what());
  }
}

void FileTransferManager::cleanupSessions(uint64_t now_seconds, uint64_t ttl_seconds, std::size_t max_sessions) {
  // 周期清理入口：先 TTL，再上限淘汰。
  std::lock_guard<std::mutex> lk(mu_);

  // 1) TTL清理。
  std::vector<uint64_t> expired_ids;
  for (const auto& kv : g_states) {
    const FileState& st = kv.second;
    if (now_seconds > st.last_update_ts && (now_seconds - st.last_update_ts) > ttl_seconds) {
      expired_ids.push_back(kv.first);
    }
  }

  for (uint64_t file_id : expired_ids) {
    auto it = g_states.find(file_id);
    if (it == g_states.end()) continue;
    if (it->second.fd >= 0) ::close(it->second.fd);
    ::unlink(it->second.temp_path.c_str());
    g_states.erase(it);
    LOG_WARN("文件模块", "清理过期文件会话，file_id=" + std::to_string(file_id));
  }

  // 2) 上限淘汰（按 last_update_ts 从旧到新）。
  if (g_states.size() <= max_sessions) return;

  std::vector<std::pair<uint64_t, uint64_t>> sortable;  // file_id, last_update_ts
  sortable.reserve(g_states.size());
  for (const auto& kv : g_states) {
    sortable.push_back(std::make_pair(kv.first, kv.second.last_update_ts));
  }
  std::sort(sortable.begin(), sortable.end(),
            [](const std::pair<uint64_t, uint64_t>& a, const std::pair<uint64_t, uint64_t>& b) {
              return a.second < b.second;
            });

  std::size_t remove_count = g_states.size() - max_sessions;
  for (std::size_t i = 0; i < remove_count; ++i) {
    uint64_t file_id = sortable[i].first;
    auto it = g_states.find(file_id);
    if (it == g_states.end()) continue;
    if (it->second.fd >= 0) ::close(it->second.fd);
    ::unlink(it->second.temp_path.c_str());
    g_states.erase(it);
    LOG_WARN("文件模块", "会话数超限，淘汰最旧文件会话，file_id=" + std::to_string(file_id));
  }
}

