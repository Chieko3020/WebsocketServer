#!/usr/bin/env bash
# 文件作用说明：
# 生成本地开发/实验用自签名 TLS 证书与私钥。
# 用法：./scripts/gen_cert.sh [输出目录]，默认输出到 certs/。
set -euo pipefail

# 读取输出目录参数；未传则使用默认 certs。
OUT_DIR="${1:-certs}"
# 若目录不存在则创建。
mkdir -p "$OUT_DIR"

# 输出证书与私钥目标路径。
CERT="$OUT_DIR/server_cert.pem"
KEY="$OUT_DIR/server_key.pem"

echo "Generating self-signed cert:"
echo "  cert: $CERT"
echo "  key : $KEY"

openssl req -x509 -newkey rsa:2048 \
  -keyout "$KEY" \
  -out "$CERT" \
  -days 365 -nodes \
  -subj "/CN=localhost" >/dev/null
# 说明：
# -x509 表示直接生成自签名证书；
# -newkey rsa:2048 生成 2048 位 RSA 密钥；
# -nodes 不对私钥加口令，便于本地自动化测试。

echo "Done."

