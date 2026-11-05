//***********************************************
// name      : SslContextTest.cc
// brief     : SslContext 单元测试：证书装配/失败路径/安全基线（H4-U1）
// attention : 证书资产位于 test/certs/（编译定义 CODEBUS_TEST_CERT_DIR 定位，
//             不依赖 ctest 工作目录）；生成与轮换见 scripts/gen-test-certs.sh
//***********************************************
#include <string>

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include "Exception.hpp"
#include "net/ssl/SslContext.h"

namespace {

/// @brief 证书目录（编译期注入的绝对路径）
std::string CertDir() {
    return CODEBUS_TEST_CERT_DIR;
}

}  // namespace

/// [正] 服务端上下文：合法证书链+私钥装配成功
TEST(SslContextTest, LoadsServerCertificate) {
    auto ctx = bus::ssl::SslContext::createServer(CertDir() + "/server.crt", CertDir() + "/server.key");
    ASSERT_NE(ctx, nullptr);
    EXPECT_NE(ctx->raw(), nullptr);
}

/// [负] 证书文件不存在：抛 SslError（what 含 ERR 队列信息）
TEST(SslContextTest, MissingCertFileThrows) {
    EXPECT_THROW(bus::ssl::SslContext::createServer(CertDir() + "/no-such.crt", CertDir() + "/server.key"),
                 bus::Exception::SslError);
}

/// [负] 证书与私钥不配对：check_private_key 失败抛 SslError
TEST(SslContextTest, MismatchedKeyThrows) {
    EXPECT_THROW(bus::ssl::SslContext::createServer(CertDir() + "/server.crt", CertDir() + "/untrusted.key"),
                 bus::Exception::SslError);
}

/// [基线] TLS 最低版本 = 1.2（旧协议面收窄，R3）
TEST(SslContextTest, MinProtoVersionIsTLS12) {
    auto server = bus::ssl::SslContext::createServer(CertDir() + "/server.crt", CertDir() + "/server.key");
    EXPECT_EQ(SSL_CTX_get_min_proto_version(server->raw()), TLS1_2_VERSION);

    auto client = bus::ssl::SslContext::createClient();
    EXPECT_EQ(SSL_CTX_get_min_proto_version(client->raw()), TLS1_2_VERSION);
}

/// [正] 客户端上下文：默认校验 + CA 追加 + 关闭校验路径可用
TEST(SslContextTest, ClientContextDefaults) {
    auto client = bus::ssl::SslContext::createClient();
    ASSERT_NE(client, nullptr);
    ASSERT_NE(client->raw(), nullptr);

    EXPECT_NO_THROW(client->useCaFile(CertDir() + "/ca.crt"));
    EXPECT_NO_THROW(client->useCaFile(""));  // 空串回落系统默认路径

    client->disableVerify();  // WARN 留痕但可用（测试/内网场景）
    EXPECT_NE(client->raw(), nullptr);
}
