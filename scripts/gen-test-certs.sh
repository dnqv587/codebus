#!/usr/bin/env bash
#***********************************************
# name      : gen-test-certs.sh
# brief     : 生成 test/certs/ 测试证书资产（H4 TLS 轮 U1–U7 共用）
# attention : 产物检入仓库；到期（2046-11）前重跑本脚本并同步检入。
#             依赖 openssl CLI ≥ 3.0（-addext / -CAcreateserial）
#***********************************************
set -euo pipefail

CERT_DIR="$(cd "$(dirname "$0")/.." && pwd)/test/certs"
mkdir -p "$CERT_DIR"
cd "$CERT_DIR"
DAYS=7300  # 20 年（Q44 到期监控）

# 1) 自签 CA（测试信任锚）
openssl req -x509 -newkey rsa:2048 -sha256 -days "$DAYS" -nodes \
    -keyout ca.key -out ca.crt \
    -subj "/CN=codebus-test-ca" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"

# 2) 服务端证书：CA 签发，SAN 必含 DNS:localhost + IP:127.0.0.1（R12）
openssl req -newkey rsa:2048 -nodes -sha256 \
    -keyout server.key -out server.csr \
    -subj "/CN=localhost"
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' > server.ext
openssl x509 -req -sha256 -days "$DAYS" \
    -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -extfile server.ext

# 3) 不受信自签证书（同 SAN——验证失败用例隔离「不受信签发者」单一变量）
openssl req -x509 -newkey rsa:2048 -sha256 -days "$DAYS" -nodes \
    -keyout untrusted.key -out untrusted.crt \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

# 自检：CA 链验证通过 + SAN 落位
openssl verify -CAfile ca.crt server.crt
openssl x509 -in server.crt -noout -ext subjectAltName

rm -f server.csr server.ext ca.srl
echo "certs generated in $CERT_DIR"
