cmake_minimum_required(VERSION 3.16)
project(wss_server LANGUAGES CXX)

# 文件作用说明：
# 本 CMake 构建脚本用于组织整个项目的编译与链接流程。
# 主要输出目标：
# 1) wss_common     : 公共静态库（CRC32/日志/线程池/WebSocket 编解码）
# 2) wss_server     : 服务端程序（TLS1.3 + WebSocket + 文件分片服务）
# 3) wss_client     : 客户端测试工具
# 4) wss_unit_tests : 协议与算法单元测试

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 开关：是否编译单元测试目标。
option(WSS_ENABLE_TESTS "Build unit tests" ON)

# 依赖查找：OpenSSL 提供 TLS/加密能力，Threads 提供线程库。
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# 将 include 目录加入头文件搜索路径。
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Common library (WebSocket codec, CRC32, ThreadPool).
add_library(wss_common
  src/common/ConfigFile.cpp
  src/common/MemoryPool.cpp
  src/common/Crc32.cpp
  src/common/Logger.cpp
  src/common/ThreadPool.cpp
  src/ws/WebSocketCodec.cpp
)
target_link_libraries(wss_common
  OpenSSL::Crypto
  Threads::Threads
)

# 服务端目标：负责 TLS + WebSocket + 文件传输主流程。
add_executable(wss_server
  src/server/main_server.cpp
  src/server/OpenSslHelpers.cpp
  src/server/WssServer.cpp
  src/file/FileTransferManager.cpp
)
target_link_libraries(wss_server
  wss_common
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)

# 客户端目标：用于端到端联调和演示。
add_executable(wss_client
  src/client/main_client.cpp
)
target_link_libraries(wss_client
  wss_common
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)

if(WSS_ENABLE_TESTS)
  # 启用 CTest 并注册单测程序。
  enable_testing()
  add_executable(wss_unit_tests
    tests/unit_tests.cpp
  )
  target_link_libraries(wss_unit_tests
    wss_common
    OpenSSL::Crypto
    Threads::Threads
  )
  add_test(NAME wss_unit_tests COMMAND wss_unit_tests)
endif()

# 编译告警级别配置：MSVC 与 GCC/Clang 分开处理。
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -O2)
endif()

