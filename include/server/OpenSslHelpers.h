#pragma once

/*
 * 文件作用说明：
 * 本文件提供 OpenSSL 初始化辅助接口，目的是将 TLS 上下文创建逻辑从业务逻辑中剥离，
 * 让服务端和客户端在“协议版本、证书加载、错误处理”上保持统一入口。
 */

#include <openssl/ssl.h>
#include <string>

namespace openssl_helpers {

// 创建并返回服务端 SSL_CTX，加载 PEM 证书和私钥。
// minTlsVersion：12 表示 min=TLS1.2、max=TLS1.3（对照实验）；13 表示仅 TLS1.3（默认）。
SSL_CTX* createServerContext(const std::string& certFile, const std::string& keyFile, int minTlsVersion = 0);

// 在已创建好的 SSL_CTX 上生成一个客户端 SSL 会话对象。
// 该接口在本项目服务端主路径中不是必须，但保留用于后续工具扩展或测试复用。
SSL* createClientSSL(SSL_CTX* ctx);

}  // namespace openssl_helpers

