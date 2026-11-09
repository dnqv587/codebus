//***********************************************
// name      : SslSessionTest.cc
// brief     : SslSession 单元测试：双 session 内存 BIO 交叉对拍（无 socket 无 loop，
//             H4 决策 D-1/D-2 的协议层验证）
// attention : 证书位于 test/certs/（CODEBUS_TEST_CERT_DIR 定位）；慢速网络以
//             「每轮只搬运一半密文」模拟（部分传输锁语义）
//***********************************************
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "net/ssl/SslContext.h"
#include "net/ssl/SslSession.h"
#include "type.h"

using namespace bus;  // 文件大小字面量（64_KB/1_MB）在 bus 内

namespace {

using bus::ssl::Role;
using bus::ssl::SslContext;
using bus::ssl::SslSession;
using bus::ssl::Want;

std::string CertDir() {
    return CODEBUS_TEST_CERT_DIR;
}

/// @brief 服务端 ctx（默认受信证书；可换 untrusted 模拟坏证书）
SslContext::ptr MakeServerCtx(const char* cert = "server.crt", const char* key = "server.key") {
    return SslContext::createServer(CertDir() + "/" + cert, CertDir() + "/" + key);
}

/// @brief 客户端 ctx（默认对端校验 + 信任测试 CA）
SslContext::ptr MakeClientCtx() {
    auto ctx = SslContext::createClient();
    ctx->useCaFile(CertDir() + "/ca.crt");
    return ctx;
}

/// @brief 确定性测试载荷
std::vector<std::byte> MakePattern(size_t n) {
    std::vector<std::byte> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::byte>((i * 31 + i / 251) & 0xFF);
    }
    return v;
}

/// @brief 单向搬运：from 的 outgoing 取 fraction 比例喂给 to（模拟慢速网络）
/// @return 本轮搬运字节数（非空时至少 1 字节——防舍入滞留尾字节）
size_t Transfer(SslSession& from, SslSession& to, double fraction) {
    bus::Slice out = from.pullOutgoing();
    if (out.empty()) {
        return 0;
    }
    size_t n = std::max<size_t>(1, static_cast<size_t>(static_cast<double>(out.size()) * fraction));
    to.feedCiphertext(out.data(), n);
    from.skipOutgoing(n);
    return n;
}

/// @brief 双向泵至双方 established（或任一 closed——坏证书路径）
void PumpToEstablished(SslSession& a, SslSession& b, double fraction = 1.0) {
    for (int round = 0; round < 128; ++round) {
        if (a.isEstablished() && b.isEstablished()) {
            return;
        }
        if (a.state() == bus::ssl::SslState::kClosed || b.state() == bus::ssl::SslState::kClosed) {
            return;  // fatal/close_notify——由调用方断言
        }
        a.advance();
        b.advance();
        if (Transfer(a, b, fraction) > 0) {
            b.advance();
        }
        if (Transfer(b, a, fraction) > 0) {
            a.advance();
        }
    }
    FAIL() << "handshake did not settle in 128 rounds (fraction=" << fraction << ")";
}

/// @brief 单向数据投递至榨干（sender 的 outgoing 全部送达 receiver 并消费；
///        握手期收到的 alert 须经 advance 处理，故 decrypt/advance 双驱）
void DeliverAll(SslSession& sender, SslSession& receiver, double fraction = 1.0) {
    for (int round = 0; round < 8192; ++round) {
        if (Transfer(sender, receiver, fraction) == 0) {
            break;
        }
        receiver.decrypt();
        receiver.advance();
    }
    receiver.decrypt();
    receiver.advance();
}

/// @brief 收集 receiver 已解密明文并清空
std::vector<std::byte> Drain(SslSession& s) {
    bus::Slice pt = s.plaintext();
    std::vector<std::byte> out(pt.data(), pt.data() + pt.size());
    s.consumePlaintext(pt.size());
    return out;
}

void ExpectSame(const std::vector<std::byte>& got, const std::vector<std::byte>& want) {
    ASSERT_EQ(got.size(), want.size());
    EXPECT_EQ(std::memcmp(got.data(), want.data(), want.size()), 0);
}

}  // namespace

/// [正] 握手完成：双 session 对拍至双方 established
TEST(SslSessionTest, HandshakeCompletes) {
    auto serverCtx = MakeServerCtx();
    auto clientCtx = MakeClientCtx();
    SslSession client(Role::kClient, *clientCtx);
    SslSession server(Role::kServer, *serverCtx);
    PumpToEstablished(client, server);
    EXPECT_TRUE(client.isEstablished());
    EXPECT_TRUE(server.isEstablished());
}

/// [正] 首个 flight：client 首次 advance 产 ClientHello（outgoing 非空）且等待对端；
///      server 未收到任何密文时 advance 无产出
TEST(SslSessionTest, ClientFirstFlightOnAdvance) {
    auto serverCtx = MakeServerCtx();
    auto clientCtx = MakeClientCtx();
    SslSession client(Role::kClient, *clientCtx);
    SslSession server(Role::kServer, *serverCtx);

    EXPECT_EQ(client.advance(), Want::kRead);
    bus::Slice hello = client.pullOutgoing();
    EXPECT_GT(hello.size(), 0u);  // ClientHello
    client.skipOutgoing(hello.size());

    EXPECT_EQ(server.advance(), Want::kRead);
    EXPECT_EQ(server.pullOutgoing().size(), 0u);  // 无密文喂入则无产出
}

/// [负] 坏证书：服务端用不受信证书 + 客户端信任测试 CA → 客户端 fatal（留档），
///      alert 送达后服务端亦转终态
TEST(SslSessionTest, BadCertificateFails) {
    auto serverCtx = MakeServerCtx("untrusted.crt", "untrusted.key");
    auto clientCtx = MakeClientCtx();
    SslSession client(Role::kClient, *clientCtx);
    SslSession server(Role::kServer, *serverCtx);
    PumpToEstablished(client, server);

    EXPECT_EQ(client.state(), bus::ssl::SslState::kClosed);
    EXPECT_FALSE(client.fatalWhat().empty());  // ERR 队列留档（验证失败详情）

    // alert 送达后服务端收尾
    DeliverAll(client, server);
    EXPECT_EQ(server.state(), bus::ssl::SslState::kClosed);
}

/// [正] 大载荷双向：1MB 各向传输字节全等（跨多 TLS record）
TEST(SslSessionTest, LargePayloadBothDirections) {
    auto serverCtx = MakeServerCtx();
    auto clientCtx = MakeClientCtx();
    SslSession client(Role::kClient, *clientCtx);
    SslSession server(Role::kServer, *serverCtx);
    PumpToEstablished(client, server);
    ASSERT_TRUE(client.isEstablished());

    const auto payload = MakePattern(1_MB);
    ASSERT_EQ(client.encrypt(payload.data(), payload.size()), payload.size());
    ASSERT_EQ(server.encrypt(payload.data(), payload.size()), payload.size());

    // 双向交替投递（同轮各搬一半，模拟全双工交织）
    for (int round = 0; round < 8192; ++round) {
        size_t c2s = Transfer(client, server, 0.5);
        if (c2s > 0) {
            server.decrypt();
        }
        size_t s2c = Transfer(server, client, 0.5);
        if (s2c > 0) {
            client.decrypt();
        }
        if (c2s == 0 && s2c == 0) {
            break;
        }
    }
    server.decrypt();
    client.decrypt();

    ExpectSame(Drain(server), payload);
    ExpectSame(Drain(client), payload);
}

/// [正] 明文早于握手完成：encrypt 排队，握手完成点自动 flush，对端全量收到
TEST(SslSessionTest, PlaintextQueuedBeforeHandshake) {
    auto serverCtx = MakeServerCtx();
    auto clientCtx = MakeClientCtx();
    SslSession client(Role::kClient, *clientCtx);
    SslSession server(Role::kServer, *serverCtx);

    const auto payload = MakePattern(64_KB);
    ASSERT_EQ(client.encrypt(payload.data(), payload.size()), payload.size());
    EXPECT_TRUE(client.hasPendingPlaintext());  // 已排队未发出
    EXPECT_EQ(client.pendingPlaintextSize(), payload.size());

    PumpToEstablished(client, server);  // 完成点自动 flush
    ASSERT_TRUE(client.isEstablished());
    EXPECT_FALSE(client.hasPendingPlaintext());

    DeliverAll(client, server);
    ExpectSame(Drain(server), payload);
}

/// [正] 慢速网络模拟：每轮只搬运一半密文，256KB 载荷字节全等（部分传输语义）
TEST(SslSessionTest, PartialCipherTransfer) {
    auto serverCtx = MakeServerCtx();
    auto clientCtx = MakeClientCtx();
    SslSession client(Role::kClient, *clientCtx);
    SslSession server(Role::kServer, *serverCtx);
    PumpToEstablished(client, server, 0.5);  // 握手本身也走半速
    ASSERT_TRUE(client.isEstablished());

    const auto payload = MakePattern(256_KB);
    ASSERT_EQ(client.encrypt(payload.data(), payload.size()), payload.size());
    DeliverAll(client, server, 0.5);
    ExpectSame(Drain(server), payload);
}

/// [正] 优雅关闭：close_notify 双向送达，双方以 kClosedPeer 收尾（非 fatal）
TEST(SslSessionTest, CloseNotifyGraceful) {
    auto serverCtx = MakeServerCtx();
    auto clientCtx = MakeClientCtx();
    SslSession client(Role::kClient, *clientCtx);
    SslSession server(Role::kServer, *serverCtx);
    PumpToEstablished(client, server);
    ASSERT_TRUE(client.isEstablished());

    client.sendCloseNotify();
    EXPECT_EQ(client.encrypt(reinterpret_cast<const std::byte*>("late"), 4), 0u);  // 关写后不接管

    DeliverAll(client, server);
    EXPECT_EQ(server.decrypt(), Want::kClosedPeer);
    server.sendCloseNotify();  // 回执
    DeliverAll(server, client);
    EXPECT_EQ(client.decrypt(), Want::kClosedPeer);

    EXPECT_EQ(client.state(), bus::ssl::SslState::kClosed);
    EXPECT_EQ(server.state(), bus::ssl::SslState::kClosed);
    EXPECT_TRUE(client.fatalWhat().empty());  // 优雅关闭非 fatal
    EXPECT_TRUE(server.fatalWhat().empty());
}

/// [负] fatal 吸收后续调用：坏证书 fatal 后 encrypt/decrypt/advance/feed 均安全（不崩溃）
TEST(SslSessionTest, FatalAbsorbsLaterCalls) {
    auto serverCtx = MakeServerCtx("untrusted.crt", "untrusted.key");
    auto clientCtx = MakeClientCtx();
    SslSession client(Role::kClient, *clientCtx);
    SslSession server(Role::kServer, *serverCtx);
    PumpToEstablished(client, server);
    ASSERT_EQ(client.state(), bus::ssl::SslState::kClosed);

    const auto payload = MakePattern(1024);
    EXPECT_EQ(client.encrypt(payload.data(), payload.size()), 0u);  // kClosed 不接管
    EXPECT_EQ(client.decrypt(), Want::kFatal);
    EXPECT_EQ(client.advance(), Want::kFatal);
    EXPECT_NO_FATAL_FAILURE({
        client.feedCiphertext(payload.data(), payload.size());  // 吸收
        client.pullOutgoing();
        client.skipOutgoing(0);
        client.sendCloseNotify();
    });
}
